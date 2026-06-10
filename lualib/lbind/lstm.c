#include "lbind/lpub.h"

// writer 的 Lua 包装: full userdata, 内部持 stm_ctx*; __gc 调 stm_free
typedef struct lua_stm_ctx {
    stm_ctx *ctx;
} lua_stm_ctx;
// reader 的 Lua 包装: full userdata, 持 ctx* + 上次读到的 lastcopy (用于"是否更新"判断)
typedef struct lua_stm_data {
    stm_ctx *ctx;
    stm_data *lastcopy;
} lua_stm_data;

/// <summary>
/// stm.copy(writer): writer grab 一次 ctx, 返回 ctx 指针的 lightuserdata, 用于跨 task 传递
/// </summary>
/// <param name="w" type="userdata">writer 对象 (stm.new 返回)</param>
/// <returns type="lightuserdata">stm_ctx 指针; 业务通过任务消息发给 reader task, 对端用 stm.newcopy 包装</returns>
static int32_t _lstm_copy(lua_State *lua) {
    lua_stm_ctx *box = lua_touserdata(lua, 1);
    int32_t ok = (NULL != box && 0 != lua_getmetatable(lua, 1));
    if (ok) {
        ok = lua_rawequal(lua, -1, lua_upvalueindex(1));
        lua_pop(lua, 1);
    }
    luaL_argcheck(lua, ok, 1, "stm writer expected");
    stm_grab(box->ctx);
    lua_pushlightuserdata(lua, box->ctx);
    return 1;
}
/// <summary>
/// stm.new(data, sz?, copy?): 创建 writer
/// </summary>
/// <param name="data" type="string|lightuserdata">初始数据; 字符串自动取长; lightuserdata 时 sz 必填</param>
/// <param name="sz" type="integer?">数据字节数 (data 为 lightuserdata 时必填)</param>
/// <param name="copy" type="integer?">data 为 lightuserdata 时: 1=内部拷贝 (默认), 0=转移所有权</param>
/// <returns type="userdata">writer 对象; w(data,sz?) 更新, stm.copy(w) 拿 handle, w 出作用域自动 release</returns>
static int32_t _lstm_newwriter(lua_State *lua) {
    size_t sz;
    int32_t copy;
    void *data = lpub_check_buf(lua, 1, &sz, &copy);
    lua_stm_ctx *box = lua_newuserdatauv(lua, sizeof(lua_stm_ctx), 0);
    box->ctx = stm_new(data, sz, copy);
    // writer metatable 作为 luaL_setfuncs 的 upvalue 传入, 此处取出关联到 box
    lua_pushvalue(lua, lua_upvalueindex(1));
    lua_setmetatable(lua, -2);
    return 1;
}
// writer.__gc: 释放 ctx 引用; 若 ctx 内部 ref 归 0 则自动 FREE
static int32_t _lstm_deletewriter(lua_State *lua) {
    lua_stm_ctx *box = lua_touserdata(lua, 1);
    if (NULL != box->ctx) {
        stm_free(box->ctx);
        box->ctx = NULL;
    }
    return 0;
}
// writer.__call: 等同 stm_update; 用法 w(data, sz?, copy?)
static int32_t _lstm_update(lua_State *lua) {
    lua_stm_ctx *box = lua_touserdata(lua, 1);
    size_t sz;
    int32_t copy;
    // 参数从 idx=2 开始 (idx=1 是 box 自身, __call 第一个参数固定为对象)
    void *data = lpub_check_buf(lua, 2, &sz, &copy);
    stm_update(box->ctx, data, sz, copy);
    return 0;
}
/// <summary>
/// stm.newcopy(handle): 用 stm.copy 返回的 ctx lightuserdata 包装为 reader
/// </summary>
/// <param name="handle" type="lightuserdata">stm_ctx 指针 (跨 task 传递; 调用方已 stm_grab)</param>
/// <returns type="userdata">reader 对象; r(func, ud?) 读快照, r 出作用域自动 release</returns>
static int32_t _lstm_newreader(lua_State *lua) {
    LUACHECK_LUDATA(lua, 1);
    lua_stm_data *box = lua_newuserdatauv(lua, sizeof(lua_stm_data), 0);
    box->ctx = lua_touserdata(lua, 1);
    box->lastcopy = NULL;
    // reader metatable 作为 luaL_setfuncs 的 upvalue 传入
    lua_pushvalue(lua, lua_upvalueindex(1));
    lua_setmetatable(lua, -2);
    return 1;
}
// reader.__gc: 释放 ctx 引用 + lastcopy 引用
static int32_t _lstm_deletereader(lua_State *lua) {
    lua_stm_data *box = lua_touserdata(lua, 1);
    if (NULL != box->ctx) {
        stm_ungrab(box->ctx);
        box->ctx = NULL;
    }
    stm_ungrab_data(box->lastcopy);
    box->lastcopy = NULL;
    return 0;
}
// reader.__call(func, ud?): 读快照. 若与 lastcopy 相同则 return false 不调 func;
// 否则调 func(lud, sz, ud?) 处理数据, 透传 func 返回值并在首位补 boolean true
static int32_t _lstm_read(lua_State *lua) {
    lua_stm_data *box = lua_touserdata(lua, 1);
    luaL_checktype(lua, 2, LUA_TFUNCTION);
    stm_data *snap = stm_grab_data(box->ctx);
    if (snap == box->lastcopy) {
        // 与上次相同, 未更新
        stm_ungrab_data(snap);
        lua_pushboolean(lua, 0);
        return 1;
    }
    stm_ungrab_data(box->lastcopy);
    box->lastcopy = snap;
    if (NULL != snap) {
        // 栈布局技巧 : 入栈 [box, func, ud?]
        // settop=3 补 nil ud; replace(1) 把 ud 移到 idx=1; settop=2 截为 [ud, func]
        // 然后 push (lud, sz, ud) 调 func; 最后用 boolean(true) 替 idx=1 作为首返回值
        lua_settop(lua, 3);
        lua_replace(lua, 1);
        lua_settop(lua, 2);
        lua_pushlightuserdata(lua, snap->data);
        lua_pushinteger(lua, (lua_Integer)snap->sz);
        lua_pushvalue(lua, 1);
        lua_call(lua, 3, LUA_MULTRET);
        lua_pushboolean(lua, 1);
        lua_replace(lua, 1);
        return lua_gettop(lua);
    }
    // writer 已释放, ctx->data=NULL
    lua_pushboolean(lua, 0);
    return 1;
}
// srey.stm
LUAMOD_API int luaopen_stm(lua_State *lua) {
    luaL_checkversion(lua);
    lua_createtable(lua, 0, 3);
    // writer: metatable 作为 luaL_setfuncs upvalue 关联到 new/copy 函数; metatable 不进 registry
    luaL_Reg reg_writer[] = {
        { "new", _lstm_newwriter },
        { NULL, NULL }
    };
    lua_createtable(lua, 0, 2);
    lua_pushcfunction(lua, _lstm_deletewriter); lua_setfield(lua, -2, "__gc");
    lua_pushcfunction(lua, _lstm_update); lua_setfield(lua, -2, "__call");
    lua_pushvalue(lua, -1);
    lua_pushcclosure(lua, _lstm_copy, 1);
    lua_setfield(lua, -3, "copy");
    luaL_setfuncs(lua, reg_writer, 1);
    // reader: 同上
    luaL_Reg reg_reader[] = {
        { "newcopy", _lstm_newreader },
        { NULL, NULL }
    };
    lua_createtable(lua, 0, 2);
    lua_pushcfunction(lua, _lstm_deletereader); lua_setfield(lua, -2, "__gc");
    lua_pushcfunction(lua, _lstm_read); lua_setfield(lua, -2, "__call");
    luaL_setfuncs(lua, reg_reader, 1);
    return 1;
}
