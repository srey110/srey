#ifndef LPUB_H_
#define LPUB_H_

#include "lib.h"
#include "lua/lualib.h"
#include "lua/lauxlib.h"

#define CERT_FOLDER "keys" // SSL 证书文件所在子目录名
#define CUR_TASK_NAME "_curtask" // Lua 全局变量名：当前 task 指针
#define PATH_NAME "_propath" // Lua 全局变量名：程序根路径
#define PATH_SEP_NAME "_pathsep" // Lua 全局变量名：路径分隔符字符串
#define MSG_DISP_FUNC "message_dispatch" // Lua 脚本中消息分发回调函数名

// 校验栈上指定位置必须是 light userdata（任意 C 指针），否则通过 luaL_argerror 抛 Lua 错误
#define LUACHECK_LUDATA(lua, idx) \
    luaL_argcheck(lua, lua_islightuserdata(lua, idx), idx, "light userdata expected")
// 将已存在的元表关联到栈顶 userdata 对象上
#define ASSOC_MTABLE(lua, name) \
    luaL_getmetatable(lua, name);\
    lua_setmetatable(lua, -2)
// 注册元表并创建对应的 new 函数库；name 为元表名，regnew 为构造函数列表，regfunc 为成员方法列表。
// __metatable 置为元表名：普通 getmetatable 只拿到该字符串，故业务无法经它篡改共享元表
// （如覆写某个方法或 __index，会影响该类型的全部实例）。注意这挡不住 debug.setmetatable 的
// 类型混淆——debug.getmetatable 无视 __metatable，而挂元表到 userdata 本就只能靠 debug 库；
// 要防那个只能在 userdata 载荷内加类型标记，与自伤型威胁不成比例，已评估不做
#define REG_MTABLE(lua, name, regnew, regfunc)\
    luaL_newmetatable(lua, name);\
    lua_pushvalue(lua, -1);\
    lua_setfield(lua, -2, "__index");\
    luaL_setfuncs(lua, regfunc, 0);\
    lua_pushstring(lua, name);\
    lua_setfield(lua, -2, "__metatable");\
    luaL_newlib(lua, regnew)
// 声明 task_ctx *var 并从当前 Lua 全局变量取值；取不到直接 luaL_error(longjmp，不返回)
#define LPUB_CUR_TASK(lua, var) \
    task_ctx *var = global_userdata((lua), CUR_TASK_NAME); \
    if (NULL == (var)) { \
        return luaL_error((lua), "task is nil"); \
    }
// 声明 task_ctx *var 并从栈位 1 取值：nil/none 取当前 task(_curtask 全局)，否则按 lightuserdata 校验取值；
// 不做 NULL 校验，调用方按各自语义处理(luaL_error 报错或 pushnil 等)
#define LPUB_TASK_ARG(lua, var) \
    task_ctx *var; \
    int32_t _lpta_type_##var = lua_type((lua), 1); \
    if (LUA_TNIL == _lpta_type_##var || LUA_TNONE == _lpta_type_##var) { \
        (var) = global_userdata((lua), CUR_TASK_NAME); \
    } else { \
        LUACHECK_LUDATA((lua), 1); \
        (var) = lua_touserdata((lua), 1); \
    }
// 声明 type **var 并从栈位 1 按 mt 校验取值(双重指针，对应可被 __gc 提前置 NULL 的 reader/stmt 型 userdata)；
// *var 为 NULL(已被显式释放)时直接 luaL_error(longjmp，不返回)
#define LPUB_UD_ARG(lua, type, mt, var, errmsg) \
    type **var = luaL_checkudata((lua), 1, (mt)); \
    if (NULL == *(var)) { \
        return luaL_error((lua), (errmsg)); \
    }

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
/// lightuserdata: 返回指针, size 从 idx+1 读 integer(负数 argerror——转成 size_t 会变成 SIZE_MAX,
///   让下游按天文数字去读那块内存), copy(若非 NULL)从 idx+2 读 integer(缺失/非 integer 默认 1);
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
/// <summary>
/// 校验随 lightuserdata 一起传进来的字节数。负数转成 size_t 后是个天文数字:bson_iter_init 唯一的
/// 边界就是拿文档头声明的长度跟 doc.size 比,doc.size 一旦成了 SIZE_MAX 那道判定永不触发,
/// 文档头写多长就往后读多长;超 INT32_MAX 则在组包侧撞断言。两者都在这里挡成可被 pcall 捕获的 Lua 错
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="idx">长度在栈中的位置</param>
/// <returns>字节数;越界走 luaL_argerror(longjmp,不返回)</returns>
size_t lpub_check_bson_lens(lua_State *lua, int32_t idx);
/// <summary>
/// 取 BSON 二进制参数:string 自带长度;lightuserdata 从 idx+1 读长度。取值本身走 lpub_check_buf,
/// 这里只补它没有的上界——它服务的是收发缓冲,只要求非负。
/// 上界两条分支都得卡:Lua 字符串自身能远超 INT32_MAX,只卡 lightuserdata 等于给字符串留了后门。
/// lightuserdata 那条先自己把长度验一遍,是为了让越界报错统一说 BSON 的口径,而不是先撞上
/// lpub_check_buf 那句只提非负的 "size must be >= 0";验过之后 lpub 那道判定必然通过。
/// 末尾那道只对 string 分支有意义,越界的就是参数本身,故报在 idx 上。
/// 注意只挡得住"长度本身非法",挡不住"长度合法但比缓冲实际长"——(指针, 长度) 这种入参形状
/// 天然只能信调用方
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="idx">data 在栈中的位置</param>
/// <param name="lens">输出:字节数</param>
/// <returns>data 指针;不合格走 luaL_argerror(longjmp,不返回)</returns>
char *lpub_check_bson_bin(lua_State *lua, int32_t idx, size_t *lens);
/// <summary>
/// 取栈位 idx 的 task 标识：string 视为 task 名，经 task_find_name 换成句柄（查不到得 INVALID_TNAME，
/// 由调用方后续的 task_grab 判空）；其余按 integer 当句柄直取（非整数由 luaL_checkinteger 抛错）。
/// 各绑定对外都是"名字或句柄二选一"，判定收在这一处
/// </summary>
/// <param name="lua">Lua 虚拟机状态</param>
/// <param name="idx">参数在栈中的位置</param>
/// <returns>task 句柄；名字查不到时为 INVALID_TNAME</returns>
name_t lpub_task_handle(lua_State *lua, int32_t idx);
/// <summary>
/// 组包类绑定的统一收尾：pack 非空时压 (lightuserdata, 长度) 返 2，为空时压单个 nil 返 1。
/// 用法固定为 return lpub_rtn_lud(lua, pack, size);
/// 何时用它、何时改用 luaL_error：数据相关、调用方能降级的失败（载荷超协议上限、
/// 会话绑定已分叉等）走本函数返 nil，让调用方判一次；调用方契约违反、没有运行期恢复动作的
/// （如 stmt 绑定参数个数与 prepare 时声明的不符）直接 luaL_error 抛出，别让它被忽略。
/// </summary>
/// <param name="lua">Lua 虚拟机状态</param>
/// <param name="pack">组包结果；NULL 表示组包被拒</param>
/// <param name="size">pack 字节数（pack 为 NULL 时不使用）</param>
/// <returns>压栈的返回值个数：2 或 1</returns>
int32_t lpub_rtn_lud(lua_State *lua, void *pack, size_t size);
/// <summary>
/// 将 url_ctx 字段打包为 Lua 表并压栈（scheme/user/psw/host/port/path/query/segs/param）
/// </summary>
/// <param name="lua">Lua 虚拟机状态</param>
/// <param name="url">已由 url_parse 填充的 url_ctx</param>
void lpub_push_url_table(lua_State *lua, url_ctx *url);

#endif//LPUB_H_
