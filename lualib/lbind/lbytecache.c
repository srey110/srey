#include "lbind/lbytecache.h"
#if ENABLE_LUA_BYTECACHE
#include "lua/lauxlib.h"
#include "containers/hashmap.h"
#include "thread/rwlock_distr.h"
#include "utils/utils.h"
#include "base/macro.h"

// 缓存条目:path 作 key(strdup),code 为编译后字节码(MALLOC);进程级常驻
typedef struct bc_entry {
    size_t size;      // 字节码字节数
    uint64_t mtime;   // 文件修改时间;LBC_CHECK_MTIME=0 时恒 0
    char *path;       // 规范化路径,hashmap key
    char *code;       // 字节码 buffer
}bc_entry;
// lua_dump 输出累积缓冲
typedef struct dump_buf {
    size_t len;
    size_t cap;
    char *buf;
}dump_buf;

static struct hashmap *_bc_map;// path -> bc_entry
static rwlock_distr_ctx *_bc_lock;// 复用 loader->lckcache,不自建锁

// hashmap 回调:按 path 哈希/比较;元素释放 path + code
static uint64_t _lbc_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    (void)seed0;
    (void)seed1;
    const bc_entry *e = (const bc_entry *)item;
    return hash(e->path, strlen(e->path));
}
static int _lbc_compare(const void *a, const void *b, void *ud) {
    (void)ud;
    return strcmp(((const bc_entry *)a)->path, ((const bc_entry *)b)->path);
}
static void _lbc_entry_free(void *item) {
    bc_entry *e = (bc_entry *)item;
    FREE(e->path);
    FREE(e->code);
}
void lbc_init(rwlock_distr_ctx *lck) {
    _bc_lock = lck;
    _bc_map = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                         sizeof(bc_entry), ONEK, 0, 0,
                                         _lbc_hash, _lbc_compare, _lbc_entry_free, NULL);
}
void lbc_free(void) {
    if (NULL != _bc_map) {
        hashmap_free(_bc_map);
        _bc_map = NULL;
    }
}
// lua_dump writer:把字节码分片追加到 dump_buf
static int _lbc_writer(lua_State *lua, const void *p, size_t sz, void *ud) {
    (void)lua;
    dump_buf *db = (dump_buf *)ud;
    if (db->len + sz > db->cap) {
        size_t ncap = (0 == db->cap) ? 4096 : db->cap;
        while (ncap < db->len + sz) {
            ncap *= 2;
        }
        REALLOC(db->buf, db->buf, ncap);
        db->cap = ncap;
    }
    memcpy(db->buf + db->len, p, sz);
    db->len += sz;
    return 0;
}
// 写锁内调用:存/替换 path 字节码;已存在同版本则释放本次 code,mtime 变则替换
static void _lbc_put(const char *path, char *code, size_t size, uint64_t mtime) {
    bc_entry key;
    key.path = (char *)path;
    bc_entry *found = (bc_entry *)hashmap_get(_bc_map, &key);
    if (NULL != found) {
        if (found->mtime == mtime) {
            FREE(code);
            return;
        }
        FREE(found->code);
        found->code = code;
        found->size = size;
        found->mtime = mtime;
        return;
    }
    bc_entry ne;
    size_t plen = strlen(path);
    MALLOC(ne.path, plen + 1);
    memcpy(ne.path, path, plen + 1);
    ne.code = code;
    ne.size = size;
    ne.mtime = mtime;
    hashmap_set(_bc_map, &ne);
}
int32_t lbc_loadfile(lua_State *lua, const char *path) {
    bc_entry key;
    key.path = (char *)path;
    rwlock_distr_rdlock(_bc_lock);
    const bc_entry *e = (const bc_entry *)hashmap_get(_bc_map, &key);
    int32_t hit = (NULL != e);
    uint64_t mt = 0;
#if LBC_CHECK_MTIME
    mt = file_mtime(path);
    if (0 != hit && e->mtime != mt) {
        hit = 0;
    }
#endif
    int32_t st = LUA_OK;
    if (0 != hit) {
        st = luaL_loadbufferx(lua, e->code, e->size, path, "b");
    }
    rwlock_distr_runlock(_bc_lock);
    if (0 != hit) {
        return st;
    }
    if (LUA_OK != (st = luaL_loadfilex(lua, path, NULL))) {
        return st;
    }
    dump_buf db = { 0, 0, NULL };
    lua_dump(lua, _lbc_writer, &db, 0);
    rwlock_distr_wrlock(_bc_lock);
    _lbc_put(path, db.buf, db.len, mt);
    rwlock_distr_wrunlock(_bc_lock);
    return LUA_OK;
}
// require 缓存版 Lua searcher:package.searchpath 定位文件 + lbc_loadfile
static int _lbc_searcher(lua_State *lua) {
    luaL_checkstring(lua, 1);
    lua_getglobal(lua, "package");
    lua_getfield(lua, -1, "searchpath");
    lua_pushvalue(lua, 1);
    lua_getfield(lua, -3, "path");
    lua_call(lua, 2, 2);
    if (lua_isnil(lua, -2)) {
        return 1;
    }
    const char *path = lua_tostring(lua, -2);
    if (LUA_OK != lbc_loadfile(lua, path)) {
        return lua_error(lua);
    }
    lua_pushstring(lua, path);
    return 2;
}
void lbc_install_searcher(lua_State *lua) {
    lua_getglobal(lua, "package");
    lua_getfield(lua, -1, "searchers");
    lua_pushcfunction(lua, _lbc_searcher);
    lua_rawseti(lua, -2, 2);
    lua_pop(lua, 2);
}
void lbc_clear(const char *path) {
    rwlock_distr_wrlock(_bc_lock);
    if (NULL == path) {
        hashmap_clear(_bc_map, 0);
    } else {
        bc_entry key;
        key.path = (char *)path;
        bc_entry *removed = (bc_entry *)hashmap_delete(_bc_map, &key);
        if (NULL != removed) {
            _lbc_entry_free(removed);
        }
    }
    rwlock_distr_wrunlock(_bc_lock);
}

#endif
