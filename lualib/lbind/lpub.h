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

// 校验栈上指定位置必须是 light userdata（任意 C 指针），否则通过 luaL_argerror 抛 Lua 错误
#define LUACHECK_LUDATA(lua, idx) \
    luaL_argcheck(lua, lua_islightuserdata(lua, idx), idx, "light userdata expected")
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
// 收尾:push lightuserdata(pack) + push integer(size) + return 2
#define LPUB_RET_LUD(lua, pack, size) \
    lua_pushlightuserdata((lua), (pack)); \
    lua_pushinteger((lua), (lua_Integer)(size)); \
    return 2

/// <summary>
/// 从 Lua 全局变量中读取轻量用户数据（light userdata）
/// </summary>
/// <param name="lua">Lua 虚拟机状态</param>
/// <param name="name">全局变量名</param>
/// <returns>成功返回指针；变量不存在或类型不匹配时返回 NULL</returns>
void *global_userdata(lua_State *lua, const char *name);
/// <summary>
/// 从 Lua 全局变量中读取字符串值并复制到调用方栈缓冲(NUL 结尾)
/// </summary>
/// <param name="lua">Lua 虚拟机状态</param>
/// <param name="name">全局变量名</param>
/// <param name="buf">调用方提供的输出缓冲</param>
/// <param name="bufsize">buf 字节数(含 NUL 终止)</param>
/// <returns>ERR_OK 成功;ERR_FAILED 变量缺失/类型不符/buf 不足</returns>
int32_t global_string(lua_State *lua, const char *name, char *buf, size_t bufsize);
/// <summary>
/// 解析栈位 idx 的 (string|lightuserdata, size [, copy]) 参数,返回 data 指针。
/// string: 返回字符串首址, size 自动取长度, copy(若非 NULL)=1;
/// lightuserdata: 返回指针, size 从 idx+1 读 integer, copy(若非 NULL)从 idx+2 读 integer(缺失/非 integer 默认 1);
/// 其他类型: luaL_argerror(longjmp,不返回)。
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="idx">data 在栈中的位置</param>
/// <param name="size">输出: size 字节数</param>
/// <param name="copy">输出 copy 标志的指针;传 NULL 表示不解析 copy</param>
/// <returns>data 指针</returns>
/// <summary>
/// 同 lpub_check_buf,但 idx 为 in/out:返回后 *idx 推进到下一个未消费的参数位
/// (string 消费 1 位;lightuserdata 消费 data+size 2 位,copy 命中再 +1),
/// 便于在 buf 之后继续读可选参数
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="idx">输入:data 在栈中的位置;输出:下一个空闲参数位</param>
/// <param name="size">输出: size 字节数</param>
/// <param name="copy">输出 copy 标志的指针;传 NULL 表示不解析 copy</param>
/// <returns>data 指针</returns>
void *lpub_check_buf_idx(lua_State *lua, int32_t *idx, size_t *size, int32_t *copy);
void *lpub_check_buf(lua_State *lua, int32_t idx, size_t *size, int32_t *copy);

#endif//LPUB_H_
