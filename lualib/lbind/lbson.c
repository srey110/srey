#include "lbind/lpub.h"

#define MT_BSON        "_bson_ctx"
#define MT_BSON_READER "_bson_reader"
#define MT_BSON_ITER   "_bson_iter_ctx"
#define MT_BSON_OID    "_bson_oid"
#define MT_BSON_DATE   "_bson_date"
#define MT_BSON_BINARY "_bson_binary"
#define MT_BSON_INT64  "_bson_int64"
// 子类型直接写进 BSON 的一个字节(binary_set_int8)，截断出来的是另一个合法子类型
// (260 截成 4 就是 UUID)，故按线格式的位宽卡死。0x80 起是规范留给用户自定义的区间，一并放行
#define SUBTYPE_OUT_OF_RANGE "bson subtype out of range (0-255)"

typedef struct { char data[BSON_OID_LENS]; } lbson_oid_t;
typedef struct { int64_t ms; } lbson_date_t;
typedef struct { bson_subtype subtype; size_t lens; } lbson_binary_t;
// lbson_binary_t 后紧跟 lens 字节的二进制内容
typedef struct { int64_t val; } lbson_int64_t;
// bson_iter 里的 key / val / val2 / nested_doc 全是指向源 bson 那块堆缓冲的裸指针。owner 由 iter 的
// uservalue 锚住不会被 GC,所以 owner 本身恒有效;会失效的是它手里那块 buffer——:free() 置空,
// 建完 iter 又往源对象写且超出容量则被 realloc 搬走,两种情况下 iter 的裸指针都指向已释放内存。
// 故连缓冲地址一起记下,每次访问前比对(见 _lbson_iter_check)。
// 挡不住的残留:realloc 原地扩容返回同一地址时地址比对不动,此时内存没被释放,但内容已被新写入
// 覆盖,iter 读出来是新数据而非建 iter 时的快照
typedef struct { bson_ctx *owner; char *data; bson_iter iter; } lbson_iter_t;

// 可写对象(bson.new() / bson.encode())与只读对象(bson.new(data, size))共用同一个 bson_ctx,
// 但挂不同元表:只读对象一旦调写入方法会走到 binary.h "external buffer is read-only" 断言,
// 直接 abort 掉整个进程,故只读元表不提供任何写入方法。本函数供两类对象都能调的方法
// (data / tostring / __gc / iter.new / decode)校验第 1 个参数;complete 是写入方是否配平
// doc_begin/end 的概念,只读元表上不提供,故仍只认可写元表
static bson_ctx *_lbson_check(lua_State *lua) {
    void *ud = lpub_test_udata(lua, 1, MT_BSON);
    if (NULL == ud) {
        ud = lpub_check_udata(lua, 1, MT_BSON_READER);
    }
    return (bson_ctx *)ud;
}
// 同上,外加"文档已闭合"判定。depth 非 0 表示还有没配对 end() 的 doc_begin(含 bson.new() 建出来的
// 隐式顶层文档),这时首 4 字节长度前缀还是 binary_init_write 里 MALLOC 出来的未初始化堆——
// _bson_append_start 的 binary_set_skip 只推进 offset 不写字节,那 4 字节唯一的写者是 bson_append_end。
// 放出去就是把进程堆内容当文档长度交给调用方,:tostring 更会照着这个长度把堆序列化进 Lua 字符串。
// 只读对象(bson.new(data,size))的 depth 恒为 0,不受影响
static bson_ctx *_lbson_check_complete(lua_State *lua) {
    bson_ctx *bson = _lbson_check(lua);
    if (NULL == BSON_DOC(bson)) {
        luaL_error(lua, "bson: document already freed");
    }
    if (0 != bson->depth) {
        luaL_error(lua, "bson: document not complete (depth=%d), missing matching end()", bson->depth);
    }
    return bson;
}
// 可写对象专用的第 1 参数校验:除了元表,还得挡住已经 :free() 掉的对象。
// binary_free 只把 data 置空、size/offset 归零,却留着 inc,于是下一次写入照样能扩容出一块新缓冲、
// 一声不吭地写进去;而这块新缓冲从没经过 bson.new() 那次 _bson_append_start,开头 4 字节长度前缀
// 与结尾的 EOD 都没有,:data() 那两道守卫(非空 + depth==0)又恰好都能过,最后交出去的是一份
// 结构非法的文档,对端解析失败才暴露,原有字段也随旧缓冲一起没了
static bson_ctx *_lbson_check_writable(lua_State *lua) {
    bson_ctx *bson = lpub_check_udata(lua, 1, MT_BSON);
    if (NULL == BSON_DOC(bson)) {
        luaL_error(lua, "bson: document already freed");
    }
    return bson;
}
// 文档字节数:可写对象取已写入的 doc.offset(doc.size 是含扩容余量的容量);只读对象是外部托管
// 缓冲(inc==0),binary_init_read 恒把 offset 置 0,真实长度只在 doc.size 里,且不受 iter 推进影响
static size_t _lbson_lens(bson_ctx *bson) {
    return 0 == bson->doc.inc ? bson->doc.size : bson->doc.offset;
}
// 只读元表上所有写入方法名的占位,报 Lua 错而非任其走到 binary.h 的断言。
// 将来给可写元表加写入方法却忘了在只读元表登记,退化为 "attempt to call a nil value",
// 消息变差但同样不会 abort
/// <summary>
/// 只读对象（bson.new(data, size) 交出的那种）没有写入能力：调用本方法一律报 Lua 错，可被 pcall 捕获
/// </summary>
/// <param name="self" type="userdata">只读 bson 对象</param>
/// <param name="..." type="any">一概忽略</param>
/// <returns>无</returns>
static int32_t _lbson_readonly(lua_State *lua) {
    return luaL_error(lua, "bson: read-only document from bson.new(data, size), write methods unavailable");
}
// ---- bson builder ----
/// <summary>
/// 创建 bson 文档构建器。省略 data 得可写对象（MT_BSON，全部方法可用）；
/// 传入 data 得只读对象（MT_BSON_READER），仅有 :data / :tostring / :free，可交给
/// bson.iter.new 与 bson.decode；写入方法在只读元表上是报错占位——外部托管缓冲不可扩容，
/// 若照可写元表调下去会撞上 binary.h 的断言 abort 掉整个进程
/// </summary>
/// <param name="data" type="lightuserdata?">已有 BSON 数据指针，只读模式；省略时新建可写空文档。给了但为 NULL 会报错——那会造出一个挂着只读元表却处于未闭合态、写不了也读不了的对象</param>
/// <param name="size" type="integer?">data 提供时必填，已有数据字节数，取值 [0, INT32_MAX]，越界报错</param>
/// <returns type="_bson_ctx|_bson_reader">可写对象或只读对象</returns>
static int32_t _lbson_new(lua_State *lua) {
    bson_ctx *bson = lua_newuserdata(lua, sizeof(bson_ctx));
    if (lua_islightuserdata(lua, 1)) {
        char *data = lua_touserdata(lua, 1);
        // NULL 会落进 bson_init 的"内部托管"分支:按 lens 分配并把 depth 置 1,却挂上只读
        // 元表——写方法全是报错桩、end() 也调不到,产出永远闭合不了、取值全失败的对象
        luaL_argcheck(lua, NULL != data, 1, "bson data must not be NULL");
        bson_init(bson, data, lpub_check_lens(lua, 2, INT32_MAX));
        ASSOC_MTABLE(lua, MT_BSON_READER);
    } else {
        bson_init(bson, NULL, 0);
        ASSOC_MTABLE(lua, MT_BSON);
    }
    return 1;
}
/// <summary>
/// 释放内部 doc.data（仅 owned=1 时生效；同时作为 __gc / free 调用）。
/// 可重复调用；释放后再调任何写入方法一律报错，不会静默写出一份缺长度前缀与 EOD 的非法文档
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <returns>无</returns>
static int32_t _lbson_free(lua_State *lua) {
    bson_ctx *bson = _lbson_check(lua);
    BSON_FREE(bson);
    return 0;
}
// 取 cstring 字段(e_name、regex 的 pattern 与 options)。BSON 的 cstring 装不下内嵌 NUL：
// 放过去会被按 strlen 截断,"a\0b" 与 "a\0c" 塌缩成同一个值,解回来只剩最后写进去的那个
static const char *_lbson_check_cstr(lua_State *lua, int32_t idx, const char *what) {
    size_t lens;
    const char *s = luaL_checklstring(lua, idx, &lens);
    luaL_argcheck(lua, NULL == memchr(s, '\0', lens), idx, what);
    return s;
}
// 取字段名，判据同 _lbson_check_cstr
static const char *_lbson_check_key(lua_State *lua, int32_t idx) {
    return _lbson_check_cstr(lua, idx, "bson key must not contain NUL");
}
// 判一段缓冲是不是一篇完整 BSON 文档。判据同 bson_cat,只是这里要求头声明长度与缓冲严格相等
// ——append_document 把整段原样写进去,尾部多出来的字节会被解码方当成下一个元素。
// 不合格的段写进去只有 type+key 没有文档体,整篇 BSON 从这个元素起就解不开
static void _lbson_check_doc(lua_State *lua, int32_t idx, const char *doc, size_t lens) {
    if (NULL != doc
        && lens >= 5
        && (size_t)read_le32(doc) == lens
        && 0 == doc[lens - 1]) {
        return;
    }
    luaL_argerror(lua, idx, "not a complete bson document"
        " (need >= 5 bytes, header length == buffer length, last byte EOD)");
}
/// <summary>
/// 开始写入嵌套文档字段（须配对调用 end()）
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名；不得含内嵌 NUL（BSON 的 e_name 是 cstring）</param>
/// <returns>无</returns>
static int32_t _lbson_doc_begin(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    const char *key = _lbson_check_key(lua, 2);
    bson_append_document_begain(bson, key);
    return 0;
}
/// <summary>
/// 开始写入数组字段（须配对调用 end()）
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名；不得含内嵌 NUL（BSON 的 e_name 是 cstring）</param>
/// <returns>无</returns>
static int32_t _lbson_arr_begin(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    const char *key = _lbson_check_key(lua, 2);
    bson_append_array_begain(bson, key);
    return 0;
}
/// <summary>
/// 结束当前嵌套层级（写入 EOD 并回填长度）
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <returns>无</returns>
static int32_t _lbson_end(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    bson_append_end(bson);
    return 0;
}
/// <summary>
/// 追加 double 字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名；不得含内嵌 NUL（BSON 的 e_name 是 cstring）</param>
/// <param name="val" type="number">double 值</param>
/// <returns>无</returns>
static int32_t _lbson_double(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    const char *key = _lbson_check_key(lua, 2);
    double val = luaL_checknumber(lua, 3);
    bson_append_double(bson, key, val);
    return 0;
}
/// <summary>
/// 追加 UTF-8 字符串字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名；不得含内嵌 NUL（BSON 的 e_name 是 cstring）</param>
/// <param name="val" type="string">UTF-8 字符串值</param>
/// <returns>无</returns>
static int32_t _lbson_utf8(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    const char *key = _lbson_check_key(lua, 2);
    size_t vlen;
    const char *val = luaL_checklstring(lua, 3, &vlen);
    bson_append_utf8_n(bson, key, val, vlen);
    return 0;
}
/// <summary>
/// 追加已序列化 BSON 文档字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名；不得含内嵌 NUL（BSON 的 e_name 是 cstring）</param>
/// <param name="doc" type="string|lightuserdata">序列化 BSON 文档；字符串时长度自动取得。必须是一篇完整文档：至少 5 字节、头声明长度等于缓冲长度、末字节为 EOD，否则报错</param>
/// <param name="lens" type="integer?">doc 为 lightuserdata 时必填，取值 [0, INT32_MAX]，越界报错</param>
/// <returns>无</returns>
static int32_t _lbson_append_doc(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    const char *key = _lbson_check_key(lua, 2);
    size_t lens;
    char *doc = lpub_check_buf(lua, 3, &lens, NULL);
    _lbson_check_doc(lua, 3, doc, lens);
    bson_append_document(bson, key, doc, lens);
    return 0;
}
/// <summary>
/// 追加已序列化 BSON 数组字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名；不得含内嵌 NUL（BSON 的 e_name 是 cstring）</param>
/// <param name="doc" type="string|lightuserdata">序列化 BSON 数组；字符串时长度自动取得。判据同 append_doc 的 doc</param>
/// <param name="lens" type="integer?">doc 为 lightuserdata 时必填，取值 [0, INT32_MAX]，越界报错</param>
/// <returns>无</returns>
static int32_t _lbson_append_arr(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    const char *key = _lbson_check_key(lua, 2);
    size_t lens;
    char *doc = lpub_check_buf(lua, 3, &lens, NULL);
    _lbson_check_doc(lua, 3, doc, lens);
    bson_append_array(bson, key, doc, lens);
    return 0;
}
/// <summary>
/// 追加二进制数据字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名；不得含内嵌 NUL（BSON 的 e_name 是 cstring）</param>
/// <param name="subtype" type="integer">bson_subtype 枚举值，取值 [0, 255]（0x80 起为自定义区间），越界报错</param>
/// <param name="data" type="string|lightuserdata">二进制数据；字符串时长度自动取得</param>
/// <param name="lens" type="integer?">data 为 lightuserdata 时必填，取值 [0, INT32_MAX]，越界报错</param>
/// <returns>无</returns>
static int32_t _lbson_binary(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    const char *key = _lbson_check_key(lua, 2);
    bson_subtype subtype = (bson_subtype)lpub_check_u8(lua, 3, SUBTYPE_OUT_OF_RANGE);
    size_t lens;
    char *data = lpub_check_buf(lua, 4, &lens, NULL);
    bson_append_binary(bson, key, subtype, data, lens);
    return 0;
}
/// <summary>
/// 追加 ObjectId 字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名；不得含内嵌 NUL（BSON 的 e_name 是 cstring）</param>
/// <param name="oid" type="string|lightuserdata">12 字节 ObjectId</param>
/// <param name="size" type="integer?">oid 为 lightuserdata 时必填，缓冲字节数</param>
/// <returns>无</returns>
static int32_t _lbson_oid(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    const char *key = _lbson_check_key(lua, 2);
    size_t lens;
    char *oid = lpub_check_buf(lua, 3, &lens, NULL);
    // bson_append_oid 固定按 BSON_OID_LENS 字节读，短缓冲触发 OOB 读
    luaL_argcheck(lua, BSON_OID_LENS == lens, 3, "OID must be 12 bytes");
    bson_append_oid(bson, key, oid);
    return 0;
}
/// <summary>
/// 追加布尔字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名；不得含内嵌 NUL（BSON 的 e_name 是 cstring）</param>
/// <param name="val" type="boolean">布尔值</param>
/// <returns>无</returns>
static int32_t _lbson_bool(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    const char *key = _lbson_check_key(lua, 2);
    int8_t b = (int8_t)lua_toboolean(lua, 3);
    bson_append_bool(bson, key, b);
    return 0;
}
/// <summary>
/// 追加 UTC 毫秒时间戳字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名；不得含内嵌 NUL（BSON 的 e_name 是 cstring）</param>
/// <param name="ms" type="integer">UTC 毫秒时间戳</param>
/// <returns>无</returns>
static int32_t _lbson_date(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    const char *key = _lbson_check_key(lua, 2);
    int64_t ms = (int64_t)luaL_checkinteger(lua, 3);
    bson_append_date(bson, key, ms);
    return 0;
}
/// <summary>
/// 追加 null 字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名；不得含内嵌 NUL（BSON 的 e_name 是 cstring）</param>
/// <returns>无</returns>
static int32_t _lbson_null(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    const char *key = _lbson_check_key(lua, 2);
    bson_append_null(bson, key);
    return 0;
}
/// <summary>
/// 追加正则表达式字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名；不得含内嵌 NUL（BSON 的 e_name 是 cstring）</param>
/// <param name="pattern" type="string">正则模式</param>
/// <param name="options" type="string">选项字符串（如 "i"、"m"）</param>
/// <returns>无</returns>
static int32_t _lbson_regex(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    const char *key = _lbson_check_key(lua, 2);
    const char *pattern = _lbson_check_cstr(lua, 3, "bson regex pattern must not contain NUL");
    const char *options = _lbson_check_cstr(lua, 4, "bson regex options must not contain NUL");
    bson_append_regex(bson, key, pattern, options);
    return 0;
}
/// <summary>
/// 追加 JavaScript 代码字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名；不得含内嵌 NUL（BSON 的 e_name 是 cstring）</param>
/// <param name="code" type="string">JavaScript 代码</param>
/// <returns>无</returns>
static int32_t _lbson_jscode(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    const char *key = _lbson_check_key(lua, 2);
    size_t clen;
    const char *code = luaL_checklstring(lua, 3, &clen);
    bson_append_jscode_n(bson, key, code, clen);
    return 0;
}
/// <summary>
/// 追加 int32 字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名；不得含内嵌 NUL（BSON 的 e_name 是 cstring）</param>
/// <param name="val" type="integer">int32 值</param>
/// <returns>无</returns>
static int32_t _lbson_int32(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    const char *key = _lbson_check_key(lua, 2);
    int32_t val = lpub_check_i32(lua, 3, "int32 out of range");
    bson_append_int32(bson, key, val);
    return 0;
}
/// <summary>
/// 追加 BSON Timestamp 字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名；不得含内嵌 NUL（BSON 的 e_name 是 cstring）</param>
/// <param name="ts" type="integer">秒级时间戳</param>
/// <param name="inc" type="integer">同秒内自增量</param>
/// <returns>无</returns>
static int32_t _lbson_timestamp(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    const char *key = _lbson_check_key(lua, 2);
    uint32_t ts = lpub_check_u32(lua, 3, "timestamp out of range");
    uint32_t inc = lpub_check_u32(lua, 4, "increment out of range");
    bson_append_timestamp(bson, key, ts, inc);
    return 0;
}
/// <summary>
/// 追加 int64 字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名；不得含内嵌 NUL（BSON 的 e_name 是 cstring）</param>
/// <param name="val" type="integer">int64 值</param>
/// <returns>无</returns>
static int32_t _lbson_int64(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    const char *key = _lbson_check_key(lua, 2);
    int64_t val = (int64_t)luaL_checkinteger(lua, 3);
    bson_append_int64(bson, key, val);
    return 0;
}
/// <summary>
/// 追加 MinKey 字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名；不得含内嵌 NUL（BSON 的 e_name 是 cstring）</param>
/// <returns>无</returns>
static int32_t _lbson_minkey(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    const char *key = _lbson_check_key(lua, 2);
    bson_append_minkey(bson, key);
    return 0;
}
/// <summary>
/// 追加 MaxKey 字段
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="key" type="string">字段名；不得含内嵌 NUL（BSON 的 e_name 是 cstring）</param>
/// <returns>无</returns>
static int32_t _lbson_maxkey(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    const char *key = _lbson_check_key(lua, 2);
    bson_append_maxkey(bson, key);
    return 0;
}
/// <summary>
/// 将另一个已完成 BSON 文档的内容拼接到当前文档
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <param name="doc" type="string|lightuserdata">已完成 BSON 文档</param>
/// <param name="size" type="integer?">doc 为 lightuserdata 时必填，buffer 字节数，取值 [0, INT32_MAX]，越界报错</param>
/// <returns>无；doc 不是落在缓冲内的完整文档时报错（缓冲不足 5 字节、末字节不是 EOD、
/// 头声明长度超出缓冲，三种情形内容都整篇丢弃，不静默）。本层不设字节数上限——
/// 那取决于承载协议，由上层判（如 mongo 侧的 MONGO_MAX_PACK_LENS）</returns>
static int32_t _lbson_cat(lua_State *lua) {
    bson_ctx *bson = _lbson_check_writable(lua);
    size_t actual_lens;
    char *doc = lpub_check_buf(lua, 2, &actual_lens, NULL);
    if (0 == actual_lens) {
        return luaL_error(lua, "bson_cat: empty document (need at least 5 bytes)");
    }
    if (ERR_OK != bson_cat(bson, doc, actual_lens)) {
        return luaL_error(lua, "bson_cat: document rejected, buffer %I bytes"
            " (need at least 5 bytes, header length in [5, buffer] and last byte EOD)",
            (lua_Integer)actual_lens);
    }
    return 0;
}
/// <summary>
/// 检查文档是否已完整写入（depth 为 0）
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <returns type="boolean">完整 true，否则 false；已 :free() 的对象返回 false 而不是报错</returns>
static int32_t _lbson_complete(lua_State *lua) {
    // 纯查询不该抛错，所以不走 _lbson_check_writable：bson_complete 只读 depth 与 doc.offset，
    // 不碰 doc.data，而 :free() 走的 binary_free 把 offset 清成 0，free 后求值天然得到 false
    bson_ctx *bson = lpub_check_udata(lua, 1, MT_BSON);
    return lpub_rtn_bool(lua, bson_complete(bson));
}
/// <summary>
/// 取内部 BSON 数据
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <returns type="lightuserdata">数据指针（bson 内部缓冲的**借用**指针，随 bson 对象释放而失效，
/// 调用方既不拥有它、也不能对它调 utils.ud_free 或以 copy=0 交给 srey.send）</returns>
/// <returns type="integer">字节数</returns>
static int32_t _lbson_data(lua_State *lua) {
    bson_ctx *bson = _lbson_check_complete(lua);
    return lpub_rtn_lud(lua, BSON_DOC(bson), _lbson_lens(bson));
}
/// <summary>
/// 将当前文档转换为可读字符串
/// </summary>
/// <param name="self" type="userdata">bson 对象</param>
/// <returns type="string?">可读字符串；转换失败返回 nil</returns>
static int32_t _lbson_tostring(lua_State *lua) {
    bson_ctx *bson = _lbson_check_complete(lua);
    char *str = bson_tostring(bson);
    if (NULL == str) {
        return lpub_rtn_nil(lua, 1);
    }
    lua_pushstring(lua, str);
    FREE(str);
    return 1;
}
/// <summary>
/// 生成一个新的 ObjectId
/// </summary>
/// <param>无</param>
/// <returns type="string">12 字节 ObjectId</returns>
static int32_t _lbson_gen_oid(lua_State *lua) {
    char oid[BSON_OID_LENS];
    bson_oid(oid);
    lua_pushlstring(lua, oid, BSON_OID_LENS);
    return 1;
}
/// <summary>
/// 取空 BSON 文档数据（指针为静态存储，勿释放）
/// </summary>
/// <param>无</param>
/// <returns type="lightuserdata">空文档数据指针</returns>
/// <returns type="integer">数据长度</returns>
static int32_t _lbson_empty(lua_State *lua) {
    size_t lens;
    const char *data = bson_empty(&lens);
    return lpub_rtn_lud(lua, (void *)data, lens);
}
/// <summary>
/// 将原始 BSON 数据转换为可读字符串
/// </summary>
/// <param name="data" type="string|lightuserdata">BSON 数据；字符串时长度自动取得</param>
/// <param name="lens" type="integer?">data 为 lightuserdata 时必填，取值 [0, INT32_MAX]，越界报错</param>
/// <returns type="string?">可读字符串；转换失败返回 nil</returns>
static int32_t _lbson_tostring2(lua_State *lua) {
    size_t lens;
    char *data = lpub_check_buf(lua, 1, &lens, NULL);
    char *str = bson_tostring2(data, lens);
    if (NULL == str) {
        return lpub_rtn_nil(lua, 1);
    }
    lua_pushstring(lua, str);
    FREE(str);
    return 1;
}
/// <summary>
/// 将 bson_type 枚举整数转换为可读字符串
/// </summary>
/// <param name="type" type="integer">bson_type 枚举值</param>
/// <returns type="string">可读类型名</returns>
static int32_t _lbson_type_tostring(lua_State *lua) {
    bson_type type = (bson_type)luaL_checkinteger(lua, 1);
    lua_pushstring(lua, bson_type_tostring(type));
    return 1;
}
/// <summary>
/// 将 bson_subtype 枚举整数转换为可读字符串
/// </summary>
/// <param name="type" type="integer">bson_subtype 枚举值，取值 [0, 255]，越界报错</param>
/// <returns type="string">可读子类型名</returns>
static int32_t _lbson_subtype_tostring(lua_State *lua) {
    bson_subtype type = (bson_subtype)lpub_check_u8(lua, 1, SUBTYPE_OUT_OF_RANGE);
    lua_pushstring(lua, bson_subtype_tostring(type));
    return 1;
}
// ---- wrapper：OID ----
/// <summary>
/// 创建 OID 包装对象
/// </summary>
/// <param name="str" type="string">12 字节 ObjectId 原始数据</param>
/// <returns type="_bson_oid">OID 包装对象</returns>
static int32_t _lbson_mkoid(lua_State *lua) {
    size_t lens;
    const char *str = luaL_checklstring(lua, 1, &lens);
    luaL_argcheck(lua, lens == BSON_OID_LENS, 1, "OID must be 12 bytes");
    lbson_oid_t *ud = lua_newuserdata(lua, sizeof(lbson_oid_t));
    memcpy(ud->data, str, BSON_OID_LENS);
    ASSOC_MTABLE(lua, MT_BSON_OID);
    return 1;
}
/// <summary>
/// 取 ObjectId 的原始字节
/// </summary>
/// <param name="self" type="userdata">_bson_oid 对象</param>
/// <returns type="string">12 字节原始 OID</returns>
static int32_t _lbson_mkoid_data(lua_State *lua) {
    lbson_oid_t *ud = lpub_check_udata(lua, 1, MT_BSON_OID);
    lua_pushlstring(lua, ud->data, BSON_OID_LENS);
    return 1;
}
// ---- wrapper：DATE ----
/// <summary>
/// 创建 Date 包装对象
/// </summary>
/// <param name="ms" type="integer">UTC 毫秒时间戳</param>
/// <returns type="_bson_date">Date 包装对象</returns>
static int32_t _lbson_mkdate(lua_State *lua) {
    int64_t ms = (int64_t)luaL_checkinteger(lua, 1);
    lbson_date_t *ud = lua_newuserdata(lua, sizeof(lbson_date_t));
    ud->ms = ms;
    ASSOC_MTABLE(lua, MT_BSON_DATE);
    return 1;
}
/// <summary>
/// 取日期时间值
/// </summary>
/// <param name="self" type="userdata">_bson_date 对象</param>
/// <returns type="integer">UTC 毫秒时间戳</returns>
static int32_t _lbson_mkdate_ms(lua_State *lua) {
    lbson_date_t *ud = lpub_check_udata(lua, 1, MT_BSON_DATE);
    lua_pushinteger(lua, ud->ms);
    return 1;
}
// ---- wrapper：BINARY ----
/// <summary>
/// 创建 Binary 包装对象
/// </summary>
/// <param name="subtype" type="integer">bson_subtype 枚举值，取值 [0, 255]（0x80 起为自定义区间），越界报错</param>
/// <param name="data" type="string|lightuserdata">二进制数据；字符串时长度自动取得</param>
/// <param name="lens" type="integer?">data 为 lightuserdata 时必填，取值 [0, INT32_MAX]，越界报错</param>
/// <returns type="_bson_binary">Binary 包装对象</returns>
static int32_t _lbson_mkbinary(lua_State *lua) {
    bson_subtype subtype = (bson_subtype)lpub_check_u8(lua, 1, SUBTYPE_OUT_OF_RANGE);
    size_t lens;
    char *data = lpub_check_buf(lua, 2, &lens, NULL);
    lbson_binary_t *ud = lua_newuserdata(lua, sizeof(lbson_binary_t) + lens);
    ud->subtype = subtype;
    ud->lens = lens;
    if (0 != lens) {
        memcpy(ud + 1, data, lens);
    }
    ASSOC_MTABLE(lua, MT_BSON_BINARY);
    return 1;
}
/// <summary>
/// 取二进制子类型
/// </summary>
/// <param name="self" type="userdata">_bson_binary 对象</param>
/// <returns type="integer">bson_subtype 枚举值</returns>
static int32_t _lbson_mkbinary_subtype(lua_State *lua) {
    lbson_binary_t *ud = lpub_check_udata(lua, 1, MT_BSON_BINARY);
    lua_pushinteger(lua, ud->subtype);
    return 1;
}
/// <summary>
/// 取二进制载荷
/// </summary>
/// <param name="self" type="userdata">_bson_binary 对象</param>
/// <returns type="string">二进制内容</returns>
static int32_t _lbson_mkbinary_data(lua_State *lua) {
    lbson_binary_t *ud = lpub_check_udata(lua, 1, MT_BSON_BINARY);
    lua_pushlstring(lua, (const char *)(ud + 1), ud->lens);
    return 1;
}
// ---- wrapper：INT64 ----
/// <summary>
/// 创建 INT64 包装对象，强制以 BSON INT64 编码
/// </summary>
/// <param name="val" type="integer">int64 值</param>
/// <returns type="_bson_int64">INT64 包装对象</returns>
static int32_t _lbson_mkint64(lua_State *lua) {
    int64_t val = (int64_t)luaL_checkinteger(lua, 1);
    lbson_int64_t *ud = lua_newuserdata(lua, sizeof(lbson_int64_t));
    ud->val = val;
    ASSOC_MTABLE(lua, MT_BSON_INT64);
    return 1;
}
/// <summary>
/// 取 int64 值
/// </summary>
/// <param name="self" type="userdata">_bson_int64 对象</param>
/// <returns type="integer">int64 整数值</returns>
static int32_t _lbson_mkint64_val(lua_State *lua) {
    lbson_int64_t *ud = lpub_check_udata(lua, 1, MT_BSON_INT64);
    lua_pushinteger(lua, ud->val);
    return 1;
}
// ---- encode 辅助 ----
// 检查 Lua table 是否为纯序列（key 全为连续整数 1..n，n>0）
static int32_t _table_is_array(lua_State *lua, int32_t idx, lua_Integer *n) {
    lua_Integer len = (lua_Integer)lua_rawlen(lua, idx);
    lua_Integer count;
    lua_Integer k;
    *n = len;
    if (0 == len) {
        return 0;
    }
    count = 0;
    lua_pushnil(lua);
    while (lua_next(lua, idx)) {
        lua_pop(lua, 1);// 弹 value,留 key 在栈顶
        // key 必须是落在 [1, len] 内的整数。只数个数的话,"洞的个数 == 额外命名 key 的个数"
        // 时会把混合表误判成纯序列,那些命名 key 在按下标写出时被静默丢掉
        if (!lua_isinteger(lua, -1)) {
            lua_pop(lua, 1);
            return 0;
        }
        k = lua_tointeger(lua, -1);
        if (k < 1
            || k > len) {
            lua_pop(lua, 1);
            return 0;
        }
        count++;
    }
    // key 全在 [1, len] 内且个数正好 len,按鸽巢即 1..len 无重无洞
    return count == len;
}
static void _lbson_encode_value(lua_State *lua, int32_t val_idx, bson_ctx *bson, const char *key);
static void _lbson_encode_table_as_doc(lua_State *lua, int32_t idx, bson_ctx *bson) {
    luaL_checkstack(lua, 4, "bson encode");
    const char *key;
    char keybuf[24];
    size_t klens;
    lua_pushnil(lua);
    while (lua_next(lua, idx)) {
        key = NULL;
        if (LUA_TSTRING == lua_type(lua, -2)) {
            // 已判过是 string,lua_tolstring 不会就地转换,不破坏 lua_next 的遍历
            key = lua_tolstring(lua, -2, &klens);
            if (NULL != memchr(key, '\0', klens)) {
                luaL_error(lua, "bson encode: key must not contain NUL");
            }
        } else if (lua_isinteger(lua, -2)) {
            key = lpub_int_str(keybuf, sizeof(keybuf), lua_tointeger(lua, -2), NULL);
        } else {
            // 与下方 value 类型不支持时的处理一致：报错，不静默丢弃该键值对
            luaL_error(lua, "bson encode unsupported key type '%s'", lua_typename(lua, lua_type(lua, -2)));
        }
        _lbson_encode_value(lua, lua_gettop(lua), bson, key);
        lua_pop(lua, 1);
    }
}
static void _lbson_encode_table_as_arr(lua_State *lua, int32_t idx, bson_ctx *bson, lua_Integer n) {
    lua_Integer i;
    char keybuf[24];
    const char *key;
    for (i = 1; i <= n; i++) {
        key = lpub_int_str(keybuf, sizeof(keybuf), i - 1, NULL);
        lua_rawgeti(lua, idx, i);
        _lbson_encode_value(lua, lua_gettop(lua), bson, key);
        lua_pop(lua, 1);
    }
}
static void _lbson_encode_value(lua_State *lua, int32_t val_idx, bson_ctx *bson, const char *key) {
    switch (lua_type(lua, val_idx)) {
    case LUA_TNIL:
        bson_append_null(bson, key);
        break;
    case LUA_TBOOLEAN:
        bson_append_bool(bson, key, (int8_t)lua_toboolean(lua, val_idx));
        break;
    case LUA_TNUMBER:
        if (lua_isinteger(lua, val_idx)) {
            lua_Integer iv = lua_tointeger(lua, val_idx);
            if (iv >= INT32_MIN && iv <= INT32_MAX) {
                bson_append_int32(bson, key, (int32_t)iv);
            } else {
                bson_append_int64(bson, key, (int64_t)iv);
            }
        } else {
            bson_append_double(bson, key, lua_tonumber(lua, val_idx));
        }
        break;
    case LUA_TSTRING: {
        size_t lens;
        const char *s = lua_tolstring(lua, val_idx, &lens);
        bson_append_utf8_n(bson, key, s, lens);
        break;
    }
    case LUA_TTABLE: {
        luaL_checkstack(lua, 4, "bson encode");
        lua_Integer n;
        if (_table_is_array(lua, val_idx, &n)) {
            bson_append_array_begain(bson, key);
            _lbson_encode_table_as_arr(lua, val_idx, bson, n);
            bson_append_end(bson);
        } else {
            bson_append_document_begain(bson, key);
            _lbson_encode_table_as_doc(lua, val_idx, bson);
            bson_append_end(bson);
        }
        break;
    }
    case LUA_TLIGHTUSERDATA:
        if (NULL != lua_touserdata(lua, val_idx)) {
            luaL_error(lua, "bson encode unsupported light userdata, key '%s'", key);
        }
        bson_append_null(bson, key);
        break;
    case LUA_TUSERDATA:
        // 值的元表只取一次，再逐个跟包装元表比。
        // 按常见程度排：OID 每篇文档的 _id 都有，INT64 是 decode 回写时最多的
        if (!lua_getmetatable(lua, val_idx)) {
            luaL_error(lua, "bson encode unsupported userdata, key '%s'", key);
        }
        if (lpub_is_mtable(lua, MT_BSON_OID)) {
            lbson_oid_t *ud = lua_touserdata(lua, val_idx);
            bson_append_oid(bson, key, ud->data);
        } else if (lpub_is_mtable(lua, MT_BSON_INT64)) {
            lbson_int64_t *ud = lua_touserdata(lua, val_idx);
            bson_append_int64(bson, key, ud->val);
        } else if (lpub_is_mtable(lua, MT_BSON_DATE)) {
            lbson_date_t *ud = lua_touserdata(lua, val_idx);
            bson_append_date(bson, key, ud->ms);
        } else if (lpub_is_mtable(lua, MT_BSON_BINARY)) {
            lbson_binary_t *ud = lua_touserdata(lua, val_idx);
            bson_append_binary(bson, key, ud->subtype, (char *)(ud + 1), ud->lens);
        } else {
            luaL_error(lua, "bson encode unsupported userdata, key '%s'", key);
        }
        lua_pop(lua, 1);
        break;
    default:
        luaL_error(lua, "bson encode unsupported type '%s', key '%s'", lua_typename(lua, lua_type(lua, val_idx)), key);
        break;
    }
}
/// <summary>
/// 将 Lua table 编码为 BSON 文档，返回 bson_ctx userdata；顶层始终作为 DOCUMENT，嵌套纯序列 table 作为 ARRAY。
/// nil 与 bson.null 都编成 BSON null。整数 key 串成十进制字符串当字段名，故 [1] 与 "1" 会撞成同一个
/// 字段名，本函数不查重（同 yyjson.encode），撞了就产出带重复字段的文档、回读只剩其一，调用方自己保证不撞
/// </summary>
/// <param name="t" type="table&lt;string,any&gt;|any[]">待编码的 Lua table</param>
/// <returns type="_bson_ctx">完整 bson 对象，可直接调用 :data() 传给 mongo API</returns>
static int32_t _lbson_encode(lua_State *lua) {
    luaL_checktype(lua, 1, LUA_TTABLE);
    bson_ctx *bson = lua_newuserdata(lua, sizeof(bson_ctx));
    bson_init(bson, NULL, 0);
    ASSOC_MTABLE(lua, MT_BSON);
    lua_Integer n;
    if (_table_is_array(lua, 1, &n)) {
        _lbson_encode_table_as_arr(lua, 1, bson, n);
    } else {
        _lbson_encode_table_as_doc(lua, 1, bson);
    }
    bson_append_end(bson);
    return 1;
}
// ---- decode 辅助 ----
static void _lbson_decode_document(lua_State *lua, char *data, size_t lens, int32_t is_array, int32_t depth);
// 将迭代器当前字段解码并写入栈顶 table，返回是否写入(数组下标据此推进,丢弃的元素不占位)。
// REGEX / TIMESTAMP / DECIMAL128 / MINKEY / MAXKEY 这几种 Lua 侧没有对应表示，本函数跳过并记一条 DEBUG——
// 它们能用 bson.new():regex()/:timestamp() 之类写进去，也能用 bson.iter 的同名取值器读出来，
// 只有 decode 这条路表达不了。MongoDB 应答常带 TIMESTAMP(operationTime / $clusterTime.clusterTime)
static int32_t _lbson_decode_field(lua_State *lua, bson_iter *iter, int32_t is_array, int32_t idx, int32_t depth) {
    int32_t err;
    if (is_array) {
        // BSON 数组 key 是 "0","1"...，转为 Lua 1-base 整数索引
        lua_pushinteger(lua, (lua_Integer)idx + 1);
    } else {
        lua_pushlstring(lua, iter->key, iter->keylens);
    }
    switch (iter->type) {
    case BSON_DOUBLE: {
        double val = bson_iter_double(iter, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return 0;
        }
        lua_pushnumber(lua, val);
        break;
    }
    case BSON_UTF8: {
        const char *val = bson_iter_utf8(iter, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return 0;
        }
        lua_pushlstring(lua, val, iter->lens);
        break;
    }
    case BSON_JSCODE: {
        const char *val = bson_iter_jscode(iter, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return 0;
        }
        lua_pushlstring(lua, val, iter->lens);
        break;
    }
    case BSON_DOCUMENT: {
        size_t dlens;
        char *ddata = bson_iter_document(iter, &dlens, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return 0;
        }
        _lbson_decode_document(lua, ddata, dlens, 0, depth + 1);
        break;
    }
    case BSON_ARRAY: {
        size_t alens;
        char *adata = bson_iter_array(iter, &alens, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return 0;
        }
        _lbson_decode_document(lua, adata, alens, 1, depth + 1);
        break;
    }
    case BSON_BINARY: {
        bson_subtype subtype;
        size_t blens;
        char *bdata = bson_iter_binary(iter, &subtype, &blens, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return 0;
        }
        lbson_binary_t *ud = lua_newuserdata(lua, sizeof(lbson_binary_t) + blens);
        ud->subtype = subtype;
        ud->lens = blens;
        if (blens > 0) {
            memcpy(ud + 1, bdata, blens);
        }
        ASSOC_MTABLE(lua, MT_BSON_BINARY);
        break;
    }
    case BSON_OID: {
        char *oid = bson_iter_oid(iter, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return 0;
        }
        lbson_oid_t *ud = lua_newuserdata(lua, sizeof(lbson_oid_t));
        memcpy(ud->data, oid, BSON_OID_LENS);
        ASSOC_MTABLE(lua, MT_BSON_OID);
        break;
    }
    case BSON_BOOL: {
        int32_t val = bson_iter_bool(iter, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return 0;
        }
        lua_pushboolean(lua, val);
        break;
    }
    case BSON_DATE: {
        int64_t ms = bson_iter_date(iter, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return 0;
        }
        lbson_date_t *ud = lua_newuserdata(lua, sizeof(lbson_date_t));
        ud->ms = ms;
        ASSOC_MTABLE(lua, MT_BSON_DATE);
        break;
    }
    case BSON_NULL:
        lua_pushlightuserdata(lua, NULL);
        break;
    case BSON_INT32: {
        int32_t val = bson_iter_int32(iter, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return 0;
        }
        lua_pushinteger(lua, val);
        break;
    }
    case BSON_INT64: {
        int64_t val = bson_iter_int64(iter, &err);
        if (ERR_OK != err) {
            lua_pop(lua, 1);
            return 0;
        }
        lbson_int64_t *ud = lua_newuserdata(lua, sizeof(lbson_int64_t));
        ud->val = val;
        ASSOC_MTABLE(lua, MT_BSON_INT64);
        break;
    }
    default:
        // 用 DEBUG 不用 WARN:TIMESTAMP 落在这一档,而 MongoDB 应答固定带 operationTime 与
        // $clusterTime.clusterTime,按 WARN 打就是每解一次应答刷两条,真告警会被淹掉
        LOG_DEBUG("bson decode: field \"%s\" type 0x%02X has no lua representation, dropped.",
                  (NULL != iter->key ? iter->key : ""), (uint32_t)iter->type);
        lua_pop(lua, 1);
        return 0;
    }
    lua_rawset(lua, -3);
    return 1;
}
static void _lbson_decode_document(lua_State *lua, char *data, size_t lens, int32_t is_array, int32_t depth) {
    luaL_checkstack(lua, 6, "bson decode");
    if (NULL == data) {
        lua_newtable(lua);
        return;
    }
    if (depth > BSON_MAX_DEPTH) {
        luaL_error(lua, "bson decode failed: nesting deeper than %d", BSON_MAX_DEPTH);
        return;// 到不了: luaL_error 会 longjmp。写出来是因为它没声明成 noreturn,落下去正好是继续递归
    }
    bson_ctx sub;
    bson_iter iter;
    int32_t cnt = 0;
    bson_init(&sub, data, lens);
    bson_iter_init(&iter, &sub);
    if (!is_array) {
        while (bson_iter_next(&iter)) {
            cnt++;
        }
        if (0 == bson_iter_error(&iter)) {
            bson_iter_reset(&iter);
        }
    }
    lua_createtable(lua, 0, cnt);
    int32_t idx = 0;
    while (bson_iter_next(&iter)) {
        idx += _lbson_decode_field(lua, &iter, is_array, idx, depth);
    }
    if (0 != bson_iter_error(&iter)) {
        // 表此刻是残缺的(数组是卡住之前的前缀,文档在计数时就出错所以是空表),原样交出去调用方分不清"文档就这么几个字段"
        // 和"后面全丢了"。两类成因都读不下去,故都报错;具体是哪类看日志。
        // sub 是只读模式,没有需要先释放的资源
        luaL_error(lua, "bson decode failed: malformed document or unsupported element type");
    }
}
/// <summary>
/// 将 BSON 数据解码为 Lua table；BSON ARRAY 字段解码为整数 key（1-base）table，DOCUMENT 解码为字符串 key table。
/// BSON null 解成 bson.null（NULL light userdata，与 yyjson.null 同值），encode 时写回 BSON null。
/// REGEX / TIMESTAMP / DECIMAL128 / MINKEY / MAXKEY 在 Lua 侧无对应表示，会被**丢弃**并逐个记一条 DEBUG
/// 日志（不是告警：Mongo 应答几乎都带 TIMESTAMP），结果表里没有那些字段；落在数组里时后续元素依次前移
/// 补上空位，故数组长度可能短于原文档，但不留空洞（留洞的话 # 与 ipairs 会在洞处截断）。
/// 要读它们请改用 bson.iter 的同名取值器（iter:regex() / iter:timestamp() …）。
/// 遍历中途卡住时报错而不是交出半截结果——那样调用方分不清"文档就这么几个字段"和"后面全丢了"。
/// 三类成因都会报错：文档结构非法、撞上本实现不认识的类型字节（几个废弃类型，理由与清单
/// 见 bson_iter_error）、嵌套超过 BSON_MAX_DEPTH；第二类另有一条 unsupported bson type 告警，据此分辨。
/// 要边遍历边自己判用 bson.iter 的 error()
/// </summary>
/// <param name="data" type="userdata|string|lightuserdata">bson_ctx userdata、Lua 字符串或 lightuserdata 指针</param>
/// <param name="lens" type="integer?">data 为 lightuserdata 时必填，字节数，取值 [0, INT32_MAX]，越界报错</param>
/// <returns type="table&lt;string,any&gt;|any[]">解码结果；BSON DOCUMENT 为字符串 key 表，BSON ARRAY 为整数 key（1-base）序列</returns>
static int32_t _lbson_decode(lua_State *lua) {
    char *data;
    size_t lens;
    if (LUA_TUSERDATA == lua_type(lua, 1)) {
        bson_ctx *bson = _lbson_check_complete(lua);
        data = BSON_DOC(bson);
        lens = _lbson_lens(bson);
    } else {
        data = lpub_check_buf(lua, 1, &lens, NULL);
    }
    _lbson_decode_document(lua, data, lens, 0, 0);
    return 1;
}
// 注册无对外库的 wrapper 元表
static void _lbson_reg_wrapper_mt(lua_State *lua, const char *name, luaL_Reg *methods) {
    lpub_new_mtable(lua, name);
    lua_pushvalue(lua, -1);
    lua_setfield(lua, -2, "__index");
    luaL_setfuncs(lua, methods, 0);
    lua_pushstring(lua, name);
    lua_setfield(lua, -2, "__metatable");
    lua_pop(lua, 1);
}
//bson
LUAMOD_API int luaopen_bson(lua_State *lua) {
    // 四个包装类型都是 POD（binary 的载荷就分配在同一块 userdata 内），故不挂 __gc：
    // 挂了只会让这些成批创建的小对象多走一轮 GC
    luaL_Reg oid_mt[] = {
        { "data",  _lbson_mkoid_data },
        { NULL, NULL }
    };
    luaL_Reg date_mt[] = {
        { "ms",    _lbson_mkdate_ms },
        { NULL, NULL }
    };
    luaL_Reg binary_mt[] = {
        { "subtype", _lbson_mkbinary_subtype },
        { "data",    _lbson_mkbinary_data },
        { NULL, NULL }
    };
    luaL_Reg int64_mt[] = {
        { "val",   _lbson_mkint64_val },
        { NULL, NULL }
    };
    _lbson_reg_wrapper_mt(lua, MT_BSON_OID,    oid_mt);
    _lbson_reg_wrapper_mt(lua, MT_BSON_DATE,   date_mt);
    _lbson_reg_wrapper_mt(lua, MT_BSON_BINARY, binary_mt);
    _lbson_reg_wrapper_mt(lua, MT_BSON_INT64,  int64_mt);
    luaL_Reg reg_new[] = {
        { "new",              _lbson_new },
        { "oid",              _lbson_gen_oid },
        { "empty",            _lbson_empty },
        { "tostring2",        _lbson_tostring2 },
        { "type_tostring",    _lbson_type_tostring },
        { "subtype_tostring", _lbson_subtype_tostring },
        { "mkoid",            _lbson_mkoid },
        { "mkdate",           _lbson_mkdate },
        { "mkbinary",         _lbson_mkbinary },
        { "mkint64",          _lbson_mkint64 },
        { "encode",           _lbson_encode },
        { "decode",           _lbson_decode },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "double",      _lbson_double },
        { "utf8",        _lbson_utf8 },
        { "doc_begin",   _lbson_doc_begin },
        { "arr_begin",   _lbson_arr_begin },
        { "end",         _lbson_end },
        { "append_doc",  _lbson_append_doc },
        { "append_arr",  _lbson_append_arr },
        { "binary",      _lbson_binary },
        { "oid",         _lbson_oid },
        { "bool",        _lbson_bool },
        { "date",        _lbson_date },
        { "null",        _lbson_null },
        { "regex",       _lbson_regex },
        { "jscode",      _lbson_jscode },
        { "int32",       _lbson_int32 },
        { "timestamp",   _lbson_timestamp },
        { "int64",       _lbson_int64 },
        { "minkey",      _lbson_minkey },
        { "maxkey",      _lbson_maxkey },
        { "cat",         _lbson_cat },
        { "complete",    _lbson_complete },
        { "data",        _lbson_data },
        { "tostring",    _lbson_tostring },
        { "free",        _lbson_free },
        { "__gc",        _lbson_free },
        { NULL, NULL }
    };
    luaL_Reg reader_mt[] = {
        { "double",      _lbson_readonly },
        { "utf8",        _lbson_readonly },
        { "doc_begin",   _lbson_readonly },
        { "arr_begin",   _lbson_readonly },
        { "end",         _lbson_readonly },
        { "append_doc",  _lbson_readonly },
        { "append_arr",  _lbson_readonly },
        { "binary",      _lbson_readonly },
        { "oid",         _lbson_readonly },
        { "bool",        _lbson_readonly },
        { "date",        _lbson_readonly },
        { "null",        _lbson_readonly },
        { "regex",       _lbson_readonly },
        { "jscode",      _lbson_readonly },
        { "int32",       _lbson_readonly },
        { "timestamp",   _lbson_readonly },
        { "int64",       _lbson_readonly },
        { "minkey",      _lbson_readonly },
        { "maxkey",      _lbson_readonly },
        { "cat",         _lbson_readonly },
        { "data",        _lbson_data },
        { "tostring",    _lbson_tostring },
        { "free",        _lbson_free },
        { "__gc",        _lbson_free },
        { NULL, NULL }
    };
    _lbson_reg_wrapper_mt(lua, MT_BSON_READER, reader_mt);
    REG_MTABLE(lua, MT_BSON, reg_new, reg_func);
    /// <field name="null" type="lightuserdata">BSON null 的哨兵，本身就是空指针（与 yyjson.null 同值）。
    /// decode 解出来的每个 BSON null 都是它，encode 时写回 BSON null；别拿去喂吃 lightuserdata 的接口</field>
    lua_pushlightuserdata(lua, NULL);
    lua_setfield(lua, -2, "null");
    return 1;
}
// ---- bson.iter ----
// 全部 iter 方法的取值口:类型校验 + 源 bson 是否已 :free()。注意不能改判 iter->doc->data ——
// bson_iter_find 会把 doc 指向 nested_doc,那是源缓冲的别名视图,源缓冲释放后
// 它仍是个非 NULL 的悬垂指针,只有源对象自己的 doc.data 会被 binary_free 置空
static bson_iter *_lbson_iter_check(lua_State *lua) {
    lbson_iter_t *wrap = lpub_check_udata(lua, 1, MT_BSON_ITER);
    if (NULL == BSON_DOC(wrap->owner)) {
        luaL_error(lua, "bson_iter: source bson already freed");
    }
    if (BSON_DOC(wrap->owner) != wrap->data) {
        luaL_error(lua, "bson_iter: source bson buffer reallocated by writes, iterator invalidated");
    }
    return &wrap->iter;
}
/// <summary>
/// 从 bson 上下文创建迭代器（以 uservalue 持有 bson 引用，防止 GC）。
/// 一次性消费契约：iter 会推进底层 bson_ctx 的 doc.offset，且 new 时强制把 offset
/// 重置到 0 以让 iter_init 正确读 doclens（encode 后 offset 在末尾，直接 init 读到 garbage）。
/// 因此对同一个**可写**对象调用 iter.new 后，它的 :data() / :complete() 不再可靠——
/// 需要保留原始数据时请先调用 :data() 取走再创建 iter，或直接走 bson.decode() 转 Lua table。
/// 只读对象（bson.new(data, size)）不受此影响：它的 :data() 取长走 doc.size，与 offset 无关。
/// 要求 bson 已闭合（depth==0，即写入模式下全部 doc_begin 均已配对 end）；否则报错。
/// iter 持有的是源缓冲的裸指针，源对象一旦 :free()，本 iter 的全部方法都改为报错
/// （可被 pcall 捕获），不会去读已释放的内存
/// </summary>
/// <param name="bson" type="_bson_ctx|_bson_reader">bson 对象，可写与只读均可</param>
/// <returns type="_bson_iter_ctx">iter 对象</returns>
static int32_t _lbson_iter_new(lua_State *lua) {
    bson_ctx *bson = _lbson_check_complete(lua);
    // 归零 doc.offset 已收进 bson_iter_init，这里不再重复
    lbson_iter_t *wrap = lua_newuserdata(lua, sizeof(lbson_iter_t));
    wrap->owner = bson;
    wrap->data = BSON_DOC(bson);
    bson_iter_init(&wrap->iter, bson);
    lua_pushvalue(lua, 1);
    lua_setiuservalue(lua, -2, 1);// 持有 bson 引用，防止 GC(owner 裸指针的有效性也靠它)
    ASSOC_MTABLE(lua, MT_BSON_ITER);
    return 1;
}
/// <summary>
/// 迭代器析构（绑定为 __gc）；迭代器不拥有任何堆内存，仅做类型校验
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns>无</returns>
static int32_t _lbson_iter_gc(lua_State *lua) {
    lpub_check_udata(lua, 1, MT_BSON_ITER);
    return 0;
}
/// <summary>
/// 将迭代器重置到文档起始位置
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns>无</returns>
static int32_t _lbson_iter_reset(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    bson_iter_reset(iter);
    return 0;
}
/// <summary>
/// 移动到下一个字段
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="boolean">有值返回 true；遍历结束返回 false</returns>
static int32_t _lbson_iter_next(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    return lpub_rtn_bool(lua, bson_iter_next(iter));
}
/// <summary>
/// 查找指定键（支持点分多级路径），找到则移动迭代器到该位置
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <param name="keys" type="string">字段路径（如 "a.b.c"）</param>
/// <returns type="boolean">找到 true，否则 false</returns>
static int32_t _lbson_iter_find(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    const char *keys = luaL_checkstring(lua, 2);
    bson_iter result;
    if (ERR_OK == bson_iter_find(iter, keys, &result)) {
        *iter = result;
        iter->doc = &iter->nested_doc;
        lua_pushboolean(lua, 1);
    } else {
        lua_pushboolean(lua, 0);
    }
    return 1;
}
/// <summary>
/// 取当前字段的 BSON 类型
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="integer">bson_type 枚举值</returns>
static int32_t _lbson_iter_type(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    lua_pushinteger(lua, iter->type);
    return 1;
}
/// <summary>
/// 取当前字段的键名
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="string?">字段名；无当前元素（尚未 next / 遍历结束 / 解析失败）返回 nil。
/// 空串是合法的 BSON 键名，不能拿它当"没有"用</returns>
static int32_t _lbson_iter_key(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    if (NULL == iter->key) {
        return lpub_rtn_nil(lua, 1);
    }
    lua_pushlstring(lua, iter->key, iter->keylens);
    return 1;
}
/// <summary>
/// 按当前字段的类型取值，口径同 bson.decode：OID / DATE / INT64 / BINARY 返回包装对象，
/// DOCUMENT / ARRAY 递归解成 table，null 返回 bson.null。注意与定型取值器不同，:oid() 返回 12 字节串
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="any">字段值；无当前元素、取值失败或类型在 Lua 侧没有表示（REGEX / TIMESTAMP 等）返回 nil。
///   DOCUMENT / ARRAY 的内层残缺或嵌套超过 BSON_MAX_DEPTH 时报错，同 bson.decode</returns>
static int32_t _lbson_iter_value(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    int32_t err = ERR_FAILED;
    size_t lens;
    char *data;
    if (NULL == iter->key) {
        return lpub_rtn_nil(lua, 1);
    }
    // 取值规则同 _lbson_decode_field，但不与它共用 switch：那边是解码热循环，多一个调用点就不再内联进循环
    switch (iter->type) {
    case BSON_DOUBLE: {
        double val = bson_iter_double(iter, &err);
        if (ERR_OK == err) {
            lua_pushnumber(lua, val);
        }
        break;
    }
    case BSON_UTF8:
        data = (char *)bson_iter_utf8(iter, &err);
        if (ERR_OK == err) {
            lua_pushlstring(lua, data, iter->lens);
        }
        break;
    case BSON_JSCODE:
        data = (char *)bson_iter_jscode(iter, &err);
        if (ERR_OK == err) {
            lua_pushlstring(lua, data, iter->lens);
        }
        break;
    case BSON_DOCUMENT:
    case BSON_ARRAY:
        data = (BSON_ARRAY == iter->type) ? bson_iter_array(iter, &lens, &err) : bson_iter_document(iter, &lens, &err);
        if (ERR_OK == err) {
            _lbson_decode_document(lua, data, lens, BSON_ARRAY == iter->type, 1);
        }
        break;
    case BSON_BINARY: {
        bson_subtype subtype;
        data = bson_iter_binary(iter, &subtype, &lens, &err);
        if (ERR_OK == err) {
            lbson_binary_t *ud = lua_newuserdata(lua, sizeof(lbson_binary_t) + lens);
            ud->subtype = subtype;
            ud->lens = lens;
            if (lens > 0) {
                memcpy(ud + 1, data, lens);
            }
            ASSOC_MTABLE(lua, MT_BSON_BINARY);
        }
        break;
    }
    case BSON_OID:
        data = bson_iter_oid(iter, &err);
        if (ERR_OK == err) {
            lbson_oid_t *ud = lua_newuserdata(lua, sizeof(lbson_oid_t));
            memcpy(ud->data, data, BSON_OID_LENS);
            ASSOC_MTABLE(lua, MT_BSON_OID);
        }
        break;
    case BSON_BOOL: {
        int32_t val = bson_iter_bool(iter, &err);
        if (ERR_OK == err) {
            lua_pushboolean(lua, val);
        }
        break;
    }
    case BSON_DATE: {
        int64_t ms = bson_iter_date(iter, &err);
        if (ERR_OK == err) {
            lbson_date_t *ud = lua_newuserdata(lua, sizeof(lbson_date_t));
            ud->ms = ms;
            ASSOC_MTABLE(lua, MT_BSON_DATE);
        }
        break;
    }
    case BSON_NULL:
        lua_pushlightuserdata(lua, NULL);
        return 1;
    case BSON_INT32: {
        int32_t val = bson_iter_int32(iter, &err);
        if (ERR_OK == err) {
            lua_pushinteger(lua, val);
        }
        break;
    }
    case BSON_INT64: {
        int64_t val = bson_iter_int64(iter, &err);
        if (ERR_OK == err) {
            lbson_int64_t *ud = lua_newuserdata(lua, sizeof(lbson_int64_t));
            ud->val = val;
            ASSOC_MTABLE(lua, MT_BSON_INT64);
        }
        break;
    }
    default:
        break;
    }
    return (ERR_OK == err) ? 1 : lpub_rtn_nil(lua, 1);
}
/// <summary>
/// 读取当前字段的 double 值
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="number?">double 值；类型不符返回 nil</returns>
static int32_t _lbson_iter_double(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    int32_t err;
    double val = bson_iter_double(iter, &err);
    if (ERR_OK != err) {
        return lpub_rtn_nil(lua, 1);
    }
    lua_pushnumber(lua, val);
    return 1;
}
/// <summary>
/// 读取当前字段的 UTF-8 字符串
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="string?">UTF-8 字符串；类型不符返回 nil</returns>
static int32_t _lbson_iter_utf8(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    int32_t err;
    const char *val = bson_iter_utf8(iter, &err);
    if (ERR_OK != err) {
        return lpub_rtn_nil(lua, 1);
    }
    lua_pushlstring(lua, val, iter->lens);
    return 1;
}
/// <summary>
/// 读取当前字段的嵌套文档数据
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="lightuserdata?">文档数据指针；类型不符返回 nil（连同后续返回值一并为 nil，共 2 个）。
/// 这是源 bson 缓冲的**借用**指针，所有权约束同 :data()</returns>
/// <returns type="integer?">字节数</returns>
static int32_t _lbson_iter_document(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    int32_t err;
    size_t lens;
    char *data = bson_iter_document(iter, &lens, &err);
    if (ERR_OK != err) {
        return lpub_rtn_nil(lua, 2);
    }
    return lpub_rtn_lud(lua, data, lens);
}
/// <summary>
/// 读取当前字段的数组数据
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="lightuserdata?">数组数据指针；类型不符返回 nil（连同后续返回值一并为 nil，共 2 个）。
/// 这是源 bson 缓冲的**借用**指针，所有权约束同 :data()</returns>
/// <returns type="integer?">字节数</returns>
static int32_t _lbson_iter_array(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    int32_t err;
    size_t lens;
    char *data = bson_iter_array(iter, &lens, &err);
    if (ERR_OK != err) {
        return lpub_rtn_nil(lua, 2);
    }
    return lpub_rtn_lud(lua, data, lens);
}
/// <summary>
/// 读取当前字段的二进制数据
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="integer?">bson_subtype 枚举值；类型不符返回 nil（连同后续返回值一并为 nil，共 3 个）</returns>
/// <returns type="lightuserdata?">数据指针；这是源 bson 缓冲的**借用**指针，所有权约束同 :data()</returns>
/// <returns type="integer?">字节数</returns>
static int32_t _lbson_iter_binary(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    int32_t err;
    size_t lens;
    bson_subtype subtype;
    char *data = bson_iter_binary(iter, &subtype, &lens, &err);
    if (ERR_OK != err) {
        return lpub_rtn_nil(lua, 3);
    }
    lua_pushinteger(lua, subtype);
    lua_pushlightuserdata(lua, data);
    lua_pushinteger(lua, (lua_Integer)lens);
    return 3;
}
/// <summary>
/// 读取当前字段的 ObjectId
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="string?">12 字节 ObjectId；类型不符返回 nil</returns>
static int32_t _lbson_iter_oid(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    int32_t err;
    char *oid = bson_iter_oid(iter, &err);
    if (ERR_OK != err) {
        return lpub_rtn_nil(lua, 1);
    }
    lua_pushlstring(lua, oid, BSON_OID_LENS);
    return 1;
}
/// <summary>
/// 读取当前字段的布尔值
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="boolean?">布尔值；类型不符返回 nil</returns>
static int32_t _lbson_iter_bool(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    int32_t err;
    int32_t val = bson_iter_bool(iter, &err);
    if (ERR_OK != err) {
        return lpub_rtn_nil(lua, 1);
    }
    return lpub_rtn_bool(lua, val);
}
/// <summary>
/// 读取当前字段的 UTC 毫秒时间戳
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="integer?">UTC 毫秒时间戳；类型不符返回 nil</returns>
static int32_t _lbson_iter_date(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    int32_t err;
    int64_t val = bson_iter_date(iter, &err);
    if (ERR_OK != err) {
        return lpub_rtn_nil(lua, 1);
    }
    lua_pushinteger(lua, val);
    return 1;
}
/// <summary>
/// 读取当前字段的正则表达式
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="string?">pattern；类型不符返回 nil（连同后续返回值一并为 nil，共 2 个）</returns>
/// <returns type="string?">options（无 options 时为 ""）</returns>
static int32_t _lbson_iter_regex(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    int32_t err;
    char *options = NULL;
    const char *pattern = bson_iter_regex(iter, &options, &err);
    if (ERR_OK != err) {
        return lpub_rtn_nil(lua, 2);
    }
    lua_pushstring(lua, pattern);
    lua_pushstring(lua, NULL != options ? options : "");
    return 2;
}
/// <summary>
/// 读取当前字段的 JavaScript 代码字符串
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="string?">JavaScript 代码；类型不符返回 nil</returns>
static int32_t _lbson_iter_jscode(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    int32_t err;
    const char *code = bson_iter_jscode(iter, &err);
    if (ERR_OK != err) {
        return lpub_rtn_nil(lua, 1);
    }
    lua_pushlstring(lua, code, iter->lens);
    return 1;
}
/// <summary>
/// 读取当前字段的 int32 值
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="integer?">int32 值；类型不符返回 nil</returns>
static int32_t _lbson_iter_int32(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    int32_t err;
    int32_t val = bson_iter_int32(iter, &err);
    if (ERR_OK != err) {
        return lpub_rtn_nil(lua, 1);
    }
    lua_pushinteger(lua, val);
    return 1;
}
/// <summary>
/// 读取当前字段的 BSON Timestamp
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="integer?">秒级时间戳；类型不符返回 nil（连同后续返回值一并为 nil，共 2 个）</returns>
/// <returns type="integer?">同秒内自增量</returns>
static int32_t _lbson_iter_timestamp(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    int32_t err;
    uint32_t inc;
    uint32_t ts = bson_iter_timestamp(iter, &inc, &err);
    if (ERR_OK != err) {
        return lpub_rtn_nil(lua, 2);
    }
    lua_pushinteger(lua, ts);
    lua_pushinteger(lua, inc);
    return 2;
}
/// <summary>
/// 读取当前字段的 int64 值
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="integer?">int64 值；类型不符返回 nil</returns>
static int32_t _lbson_iter_int64(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    int32_t err;
    int64_t val = bson_iter_int64(iter, &err);
    if (ERR_OK != err) {
        return lpub_rtn_nil(lua, 1);
    }
    lua_pushinteger(lua, val);
    return 1;
}
/// <summary>
/// 判断当前字段是否为 null
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="boolean">字段为 null 返回 true，否则 false</returns>
static int32_t _lbson_iter_isnull(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    return lpub_rtn_bool(lua, BSON_NULL == iter->type);
}
/// <summary>
/// 遍历过程中是否卡住过：next() 返回 false 有"读完"和"读不下去"两种含义，靠它区分
/// </summary>
/// <param name="self" type="userdata">iter 对象</param>
/// <returns type="boolean">中途读不下去返回 true。两类成因都算：文档结构非法，或撞上本实现
/// 不认识的类型字节（0x06 undefined / 0x0C dbpointer / 0x0E symbol / 0x0F code_w_s 这几个
/// 废弃类型）；后者另有一条 unsupported bson type 告警</returns>
static int32_t _lbson_iter_error(lua_State *lua) {
    bson_iter *iter = _lbson_iter_check(lua);
    return lpub_rtn_bool(lua, 0 != bson_iter_error(iter));
}
//bson.iter
LUAMOD_API int luaopen_bson_iter(lua_State *lua) {
    luaL_Reg reg_new[] = {
        { "new", _lbson_iter_new },
        { NULL, NULL }
    };
    luaL_Reg reg_func[] = {
        { "reset",     _lbson_iter_reset },
        { "next",      _lbson_iter_next },
        { "find",      _lbson_iter_find },
        { "type",      _lbson_iter_type },
        { "key",       _lbson_iter_key },
        { "value",     _lbson_iter_value },
        { "double",    _lbson_iter_double },
        { "utf8",      _lbson_iter_utf8 },
        { "document",  _lbson_iter_document },
        { "array",     _lbson_iter_array },
        { "binary",    _lbson_iter_binary },
        { "oid",       _lbson_iter_oid },
        { "bool",      _lbson_iter_bool },
        { "date",      _lbson_iter_date },
        { "regex",     _lbson_iter_regex },
        { "jscode",    _lbson_iter_jscode },
        { "int32",     _lbson_iter_int32 },
        { "timestamp", _lbson_iter_timestamp },
        { "int64",     _lbson_iter_int64 },
        { "isnull",    _lbson_iter_isnull },
        { "error",     _lbson_iter_error },
        { "__gc",      _lbson_iter_gc },
        { NULL, NULL }
    };
    REG_MTABLE(lua, MT_BSON_ITER, reg_new, reg_func);
    return 1;
}
