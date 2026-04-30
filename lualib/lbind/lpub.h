#ifndef LPUB_H_
#define LPUB_H_

#include "lib.h"
#include "lua/lualib.h"
#include "lua/lauxlib.h"

#define CERT_FOLDER "keys"           // SSL 证书文件所在子目录名
#define CUR_TASK_NAME "_curtask"     // Lua 全局变量名：当前 task 指针
#define PATH_NAME "_propath"         // Lua 全局变量名：程序根路径
#define PATH_SEP_NAME "_pathsep"     // Lua 全局变量名：路径分隔符字符串
#define MSG_DISP_FUNC "message_dispatch" // Lua 脚本中消息分发回调函数名

// 将已存在的元表关联到栈顶 userdata 对象上
#define ASSOC_MTABLE(lua, name) \
    luaL_getmetatable(lua, name);\
    lua_setmetatable(lua, -2)

// 注册元表并创建对应的 new 函数库；name 为元表名，regnew 为构造函数列表，regfunc 为成员方法列表
#define REG_MTABLE(lua, name, regnew, regfunc)\
    luaL_newmetatable(lua, name);\
    lua_pushvalue(lua, -1);\
    lua_setfield(lua, -2, "__index");\
    luaL_setfuncs(lua, regfunc, 0);\
    luaL_newlib(lua, regnew)

/// <summary>
/// 从 Lua 全局变量中读取轻量用户数据（light userdata）
/// </summary>
/// <param name="lua">Lua 虚拟机状态</param>
/// <param name="name">全局变量名</param>
/// <returns>成功返回指针；变量不存在或类型不匹配时返回 NULL</returns>
void *global_userdata(lua_State *lua, const char *name);
/// <summary>
/// 从 Lua 全局变量中读取字符串值
/// </summary>
/// <param name="lua">Lua 虚拟机状态</param>
/// <param name="name">全局变量名</param>
/// <returns>成功返回字符串指针；变量不存在或类型不匹配时返回 NULL</returns>
const char *global_string(lua_State *lua, const char *name);

#endif//LPUB_H_
