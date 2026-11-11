#include "lbind/lpub.h"

#define MT_HASH_RING "_hash_ring_ctx"

/// <summary>
/// 创建一致性哈希环上下文（hashring.new）
/// </summary>
/// <param>无</param>
/// <returns type="_hash_ring_ctx">哈希环对象</returns>
static int32_t _lhash_ring_new(lua_State *lua) {
    hash_ring_ctx *ring = lua_newuserdata(lua, sizeof(hash_ring_ctx));
    hash_ring_init(ring);
    ASSOC_MTABLE(lua, MT_HASH_RING);
    return 1;
}
/// <summary>
/// 释放哈希环内部资源（绑定为 __gc，由 Lua GC 自动调用）
/// </summary>
/// <param name="self" type="userdata">哈希环对象</param>
/// <returns>无</returns>
static int32_t _lhash_ring_free(lua_State *lua) {
    hash_ring_ctx *ring = lpub_check_udata(lua, 1, MT_HASH_RING);
    hash_ring_free(ring);
    return 0;
}
/// <summary>
/// 向哈希环添加节点（含虚拟节点）
/// </summary>
/// <param name="self" type="userdata">哈希环对象</param>
/// <param name="nreplicas" type="integer">虚拟节点数，须在 uint32 范围内（越界直接报错）；不在 [1, 65536] 内返回 false</param>
/// <param name="name" type="string|lightuserdata">节点名称；为字符串时长度自动取得</param>
/// <param name="lens" type="integer?">name 为 lightuserdata 时必填，表示名称字节数</param>
/// <returns type="boolean">成功 true；nreplicas 越界、name 为空串或其他失败时 false</returns>
static int32_t _lhash_ring_add(lua_State *lua) {
    hash_ring_ctx *ring = lpub_check_udata(lua, 1, MT_HASH_RING);
    uint32_t nreplicas = lpub_check_u32(lua, 2, "nreplicas out of range");
    size_t lens;
    void *name = lpub_check_buf(lua, 3, &lens, NULL);
    return lpub_rtn_bool(lua, ERR_OK == hash_ring_add(ring, name, lens, nreplicas));
}
/// <summary>
/// 从哈希环中移除指定节点（含其全部虚拟节点）
/// </summary>
/// <param name="self" type="userdata">哈希环对象</param>
/// <param name="name" type="string|lightuserdata">节点名称；为字符串时长度自动取得</param>
/// <param name="lens" type="integer?">name 为 lightuserdata 时必填，表示名称字节数</param>
/// <returns>无</returns>
static int32_t _lhash_ring_remove(lua_State *lua) {
    hash_ring_ctx *ring = lpub_check_udata(lua, 1, MT_HASH_RING);
    size_t lens;
    void *name = lpub_check_buf(lua, 2, &lens, NULL);
    hash_ring_remove(ring, name, lens);
    return 0;
}
/// <summary>
/// 按 key 在哈希环上查找最近节点
/// </summary>
/// <param name="self" type="userdata">哈希环对象</param>
/// <param name="key" type="string|lightuserdata">查找 key；为字符串时长度自动取得</param>
/// <param name="lens" type="integer?">key 为 lightuserdata 时必填，表示 key 字节数</param>
/// <returns type="string?">命中节点名称；环为空或 key 为空串时返回 nil</returns>
static int32_t _lhash_ring_find(lua_State *lua) {
    hash_ring_ctx *ring = lpub_check_udata(lua, 1, MT_HASH_RING);
    size_t lens;
    void *key = lpub_check_buf(lua, 2, &lens, NULL);
    hash_ring_node *node = hash_ring_find(ring, key, lens);
    if (NULL == node) {
        lua_pushnil(lua);
    } else {
        lua_pushlstring(lua, node->name, node->lens);
    }
    return 1;
}
/// <summary>
/// 将哈希环所有节点信息打印到 stdout（调试用途；daemon 模式下 stdout 接的是 /dev/null，看不到）
/// </summary>
/// <param name="self" type="userdata">哈希环对象</param>
/// <returns>无</returns>
static int32_t _lhash_ring_print(lua_State *lua) {
    hash_ring_ctx *ring = lpub_check_udata(lua, 1, MT_HASH_RING);
    hash_ring_print(ring);
    return 0;
}
//srey.hashring
LUAMOD_API int luaopen_hashring(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lhash_ring_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "add", _lhash_ring_add },
        { "remove", _lhash_ring_remove },
        { "find", _lhash_ring_find },
        { "print", _lhash_ring_print },
        { "__gc", _lhash_ring_free },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_HASH_RING, reg_new, reg_func);
    return 1;
}
