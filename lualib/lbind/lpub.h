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
#define PORT_OUT_OF_RANGE "port out of range" // 端口越界文案，各 connect / listen 绑定共用
#define REQTYPE_OUT_OF_RANGE "reqtype out of range" // 请求类型越界文案，core 与 harbor 绑定共用
#define LENS_RANGE "length out of range" // 长度越界文案，lpub 的取 buf 一族与 mqtt 载荷共用
#define LUDATA_NONNULL "non-null light userdata expected" // 空指针文案，下面的宏与取 buf 一族共用

// 校验栈上指定位置是 light userdata 且指针非 NULL，否则通过 luaL_argerror 抛 Lua 错误。
// 非空这一半不能省：lua_islightuserdata 对 NULL 指针也返真，而 yyjson.null 和每个解出来的
// JSON null 都是 NULL 指针，远端数据就能把它送到任何一个吃 lightuserdata 的接口上
#define LUACHECK_LUDATA(lua, idx) \
    luaL_argcheck(lua, lua_islightuserdata(lua, idx) && NULL != lua_touserdata(lua, idx), \
        idx, LUDATA_NONNULL)
// 同上但放行 NULL 指针，给那些把 NULL 当"没有这个东西"、自己判完返 nil 的入口用
#define LUACHECK_LUDATA_OPT(lua, idx) \
    luaL_argcheck(lua, lua_islightuserdata(lua, idx), idx, "light userdata expected")
// 将已存在的元表关联到栈顶 userdata 对象上
#define ASSOC_MTABLE(lua, name) \
    luaL_getmetatable(lua, name);\
    lua_setmetatable(lua, -2)
// 注册元表并创建对应的 new 函数库；name 为元表名，regnew 为构造函数列表，regfunc 为成员方法列表。
// __metatable 置为元表名：普通 getmetatable 只拿到该字符串，故业务无法经它篡改共享元表
// （如覆写某个方法或 __index，会影响该类型的全部实例）。注意这挡不住 debug.setmetatable 的
// 类型混淆——debug.getmetatable 无视 __metatable，而挂元表到 userdata 本就只能靠 debug 库
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
// 声明 type *var 并从栈位 idx 取 lightuserdata：判据同 LUACHECK_LUDATA(非 light userdata 或
// 空指针即 luaL_argerror)。这两行在各绑定里成对出现四十余次，分开写时新增入口漏掉校验那半
// 不会有编译期信号——拿到的就是个没验过的裸指针
#define LPUB_LUD_ARG(lua, type, idx, var) \
    LUACHECK_LUDATA((lua), (idx)); \
    type *var = lua_touserdata((lua), (idx))
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
/// 校验跟 lightuserdata 一起传进来的字节数。凡是 (指针, 长度) 形状的入口都从这里取长度,
/// 别再各写一句 (size_t)luaL_checkinteger。下界恒为 0：负数转成 size_t 是天文数字,
/// 会击穿下游所有"剩余长度 &lt; 需要长度"式的边界判定。
/// 上界由调用方按自己的线格式给:BSON 传 INT32_MAX;没有协议上界的传 0,只校验下界
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="idx">长度在栈中的位置</param>
/// <param name="max">允许的最大字节数;传 0 表示不校验上界</param>
/// <returns>字节数;越界走 luaL_argerror(longjmp,不返回)</returns>
size_t lpub_check_lens(lua_State *lua, int32_t idx, size_t max);
/// <summary>
/// 按目标 C 类型宽度校验并收窄。凡是要按固定字节数写进报文、或传给框架 / 数据库的值都从这里取，
/// 别再各写一句 (uintN_t)luaL_checkinteger——截断出来的是另一个合法值，组包侧无从分辨。
/// 取哪个只看目标字段的类型：u8 / u16 / u32 对应 [0,UINT8_MAX] / [0,UINT16_MAX] / [0,UINT32_MAX]，
/// i8 / i16 / i32 对应 [INT8_MIN,INT8_MAX] / [INT16_MIN,INT16_MAX] / [INT32_MIN,INT32_MAX]；
/// opt_u8 是缺省值版，参数为 none/nil 时原样返回 dft 且不校验。
/// 值域不是类型宽度的场合（flag 只许 0/1、qos 只许 0..2）仍用 lpub_check_range 写明真实上下界
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="idx">值在栈中的位置</param>
/// <param name="dft">仅 opt_u8：缺省值，参数为 none/nil 时原样返回且不校验</param>
/// <param name="what">越界时报给调用方的完整消息，如 "packet id out of range"</param>
/// <returns>该值；越界走 luaL_argerror(longjmp,不返回)</returns>
uint8_t lpub_check_u8(lua_State *lua, int32_t idx, const char *what);
uint16_t lpub_check_u16(lua_State *lua, int32_t idx, const char *what);
uint32_t lpub_check_u32(lua_State *lua, int32_t idx, const char *what);
/// <summary>
/// 取会话 id 参数,为 0 即报错。三个消费方(task_timeout / task_request / task_multi_request)
/// 都以 ASSERTAB 硬要求非 0;校验放 C 层而非脚本 wrapper 的理由同 _lcore_timeout 的 ms
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="idx">sess 在栈中的位置</param>
/// <returns>会话 id;为 0 走 luaL_argerror(longjmp,不返回)</returns>
uint64_t lpub_check_sess(lua_State *lua, int32_t idx);
/// <summary>
/// 取 0/1 开关参数。这类参数在 mqtt 与 websock 绑定里有十几处，各写一遍 lpub_check_range
/// 的 (0, 1, 文案) 时，把界写成 (0, 2) 照样编过、报错文案还照旧说"只许 0 或 1"
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="idx">值在栈中的位置</param>
/// <returns>0 或 1；其余值走 luaL_argerror(longjmp,不返回)</returns>
int32_t lpub_check_flag(lua_State *lua, int32_t idx);
/// <summary>
/// 同 lpub_check_flag，但参数可缺省（none / nil 取 dft，不做范围校验）
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="idx">值在栈中的位置</param>
/// <param name="dft">缺省值</param>
/// <returns>0 或 1；给了但不是这两个值走 luaL_argerror(longjmp,不返回)</returns>
int32_t lpub_opt_flag(lua_State *lua, int32_t idx, int32_t dft);
int8_t lpub_check_i8(lua_State *lua, int32_t idx, const char *what);
int16_t lpub_check_i16(lua_State *lua, int32_t idx, const char *what);
int32_t lpub_check_i32(lua_State *lua, int32_t idx, const char *what);
uint8_t lpub_opt_u8(lua_State *lua, int32_t idx, uint8_t dft, const char *what);
/// <summary>
/// 校验整数并收窄到 [lo, hi]。理由同上面那组按类型宽度收窄的函数：截断出来的是另一个合法值，
/// 写进报文或数据库都无从分辨。上下界不是某个 C 类型的边界时用它，是则用那一组
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="idx">值在栈中的位置</param>
/// <param name="lo">允许的下界</param>
/// <param name="hi">允许的上界</param>
/// <param name="what">越界时报给调用方的完整消息</param>
/// <returns>该值；不在 [lo, hi] 内走 luaL_argerror(longjmp,不返回)</returns>
int64_t lpub_check_range(lua_State *lua, int32_t idx, int64_t lo, int64_t hi, const char *what);
/// <summary>
/// 校验封包协议类型。凡是要交给框架/协议层的 pktype 都从这里取，别再各写一句
/// (pack_type)luaL_checkinteger——枚举外的值必须报错，不能存进 ud->pktype
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="idx">pktype 在栈中的位置</param>
/// <returns>pack_type；不在枚举内走 luaL_argerror(longjmp,不返回)</returns>
pack_type lpub_check_pktype(lua_State *lua, int32_t idx);
/// <summary>
/// 校验可选的 evssl 参数。凡是 listen / connect 那一族把 SSL 上下文当 light userdata 收的
/// 入口都从这里取，别再各写一遍 nil 判 + LUACHECK_LUDATA
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="idx">evssl 在栈中的位置</param>
/// <returns>evssl_ctx 指针；该位置为 nil 或整个缺省时返回 NULL（都表示"不用 SSL"）；
/// 传了别的又不是非空 light userdata 走 luaL_argerror(longjmp,不返回)</returns>
struct evssl_ctx *lpub_check_evssl(lua_State *lua, int32_t idx);
/// <summary>
/// 解析栈位 idx 的 (string|lightuserdata, size [, copy]) 参数,只收这两种类型。
/// string: size 取字符串长度, copy(若非 NULL)恒为 1;
/// lightuserdata: size 取自 idx+1, copy(若非 NULL)取自 idx+2, none / nil 作 1。
/// 两条分支的 size 上限同为 INT32_MAX;指针为 NULL 时 size 必须为 0。
/// 不校验 size 是否真的落在缓冲内
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="idx">data 在栈中的位置</param>
/// <param name="size">输出: size 字节数</param>
/// <param name="copy">输出 copy 标志的指针;传 NULL 表示不解析 copy</param>
/// <returns>data 指针。类型不符或上述约束不满足,一律走 luaL_argerror(longjmp,不返回)</returns>
void *lpub_check_buf(lua_State *lua, int32_t idx, size_t *size, int32_t *copy);
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
/// <summary>
/// 取可空的数据缓冲参数:string 自带长度;lightuserdata 从 idx+1 读长度,
/// 指针为 NULL 且长度非 0 时报错(口径同 lpub_check_buf)。
/// 其余类型一律报错——full userdata 是各类句柄对象,取它的载荷首址当字节缓冲会越界读。
/// 两条分支都卡 INT32_MAX,理由同 lpub_check_buf。缓冲必填的场合用它
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="idx">data 在栈中的位置;为 lightuserdata 时 idx+1 必须是长度</param>
/// <param name="size">输出:字节数,返回 NULL 时置 0。必须非 NULL,函数内裸解引用</param>
/// <returns>data 指针;入参为 nil 或无参时返回 NULL。类型不符走 luaL_argerror(longjmp,不返回)</returns>
void *lpub_opt_buf(lua_State *lua, int32_t idx, size_t *size);
/// <summary>
/// 压一个布尔返回值。与 lpub_rtn_nil / lpub_rtn_lud 同族,把"成功/失败各压一次 lua_pushboolean"
/// 的 if/else 收成一行——散着写时两种写法(先判成功 / 先判失败)并存,读的人每处都要重新确认
/// 哪个分支是成功,写反了编译不报错、返回值直接取反
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="cond">非 0 压 true,0 压 false</param>
/// <returns>返回值个数,恒为 1</returns>
int32_t lpub_rtn_bool(lua_State *lua, int32_t cond);
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
/// 取栈位置 1 的子对象在创建时锚进 uservalue 槽 1 的宿主对象指针。
/// 槽位由 uservalue 锚着，任何时候读都安全。
/// 子对象（session / stmt）存的是宿主裸指针，宿主被 obj:__gc() 提前释放后读它即
/// use-after-free，所以子对象的每个入口都得先用本函数确认宿主还活着；
/// 两处的成套写法见 LMONGO_SESSION_ARG / LMYSQL_STMT_ARG
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="omt">宿主的元表名</param>
/// <returns>宿主 C 对象指针；宿主已被 __gc 释放、槽位为空或类型不符时返回 NULL</returns>
void *lpub_owner_ptr(lua_State *lua, const char *omt);
/// <summary>
/// 压一个"载荷是单个裸指针"的 userdata 并挂上元表，栈顶即该 userdata（可紧接着
/// lua_setiuservalue 锚宿主）。LPUB_UD_ARG / lpub_owner_ptr 都按这个布局取值，
/// 载荷必须始终是一个可被 __gc 置 NULL 的指针
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="ptr">初始值；构造过程中还会失败的对象传 NULL，成功后再写回槽位</param>
/// <param name="mt">元表名（须为 MT_ 系列宏）</param>
/// <returns>userdata 内的指针槽位，按调用方自己的类型强转后写入</returns>
void **lpub_push_ud(lua_State *lua, void *ptr, const char *mt);
/// <summary>
/// 脚本侧 1 基下标转 C 的 0 基。范围判定必须排在窄化之前：截断后的值会落进合法区间
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="idx">栈位置</param>
/// <param name="count">元素总数，合法下标为 [1, count]</param>
/// <returns>0 基下标；不是整数即 luaL_checkinteger 抛错，越界返回 -1</returns>
int64_t lpub_check_index0(lua_State *lua, int32_t idx, uint64_t count);
/// <summary>
/// 同 lpub_check_range，但参数可缺省（none / nil 取 dft，不做范围校验）
/// </summary>
/// <param name="lua">Lua 栈</param>
/// <param name="idx">栈位置</param>
/// <param name="dft">缺省值</param>
/// <param name="lo">下界（含）</param>
/// <param name="hi">上界（含）</param>
/// <param name="what">越界时的报错文案</param>
/// <returns>收窄前的值；越界即 luaL_argerror（不返回）</returns>
int64_t lpub_opt_range(lua_State *lua, int32_t idx, int64_t dft, int64_t lo, int64_t hi, const char *what);
/// <summary>
/// 失败路径压 n 个 nil。全仓规矩：**失败与成功的返回值个数必须一致**——
/// 返回值被直接塞进另一个调用的实参位时，少一个就整体错位。
/// n 是个位数，不必 lua_checkstack
/// </summary>
/// <param name="lua">Lua 虚拟机状态</param>
/// <param name="n">要压的 nil 个数，须与成功路径的返回值个数相同</param>
/// <returns>压栈的返回值个数，即 n</returns>
int32_t lpub_rtn_nil(lua_State *lua, int32_t n);
/// <summary>
/// 组包类绑定的统一收尾：pack 非空时压 (lightuserdata, 长度)，为空时压 2 个 nil。
/// 何时用它、何时改用 luaL_error：数据相关、调用方能降级的失败（载荷超协议上限、
/// 会话绑定已分叉等）走本函数返 nil，让调用方判一次；调用方契约违反、没有运行期恢复动作的
/// （如 stmt 绑定参数个数与 prepare 时声明的不符）直接 luaL_error 抛出，别让它被忽略。
/// </summary>
/// <param name="lua">Lua 虚拟机状态</param>
/// <param name="pack">组包结果；NULL 表示组包被拒</param>
/// <param name="size">pack 字节数（pack 为 NULL 时不使用）</param>
/// <returns>压栈的返回值个数，恒为 2</returns>
int32_t lpub_rtn_lud(lua_State *lua, void *pack, size_t size);
/// <summary>
/// reader 取值类绑定的失败/NULL 收尾：err 为 1(字段是 SQL NULL) 压 true；其余(读取失败)压 false。
/// 读到值的分支自己压 true 加值，其余情况一律交给它。
/// 这条三态契约(ERR_OK 有值 / 1 为 NULL / 其余失败)由 mysql_reader / pgsql_reader 两侧共同产出，
/// 收在一处才不至于改契约时漏改某个字段类型
/// </summary>
/// <param name="lua">Lua 虚拟机状态</param>
/// <param name="err">reader 取值函数写回的错误码</param>
/// <returns>压栈的返回值个数，恒为 1</returns>
int32_t lpub_rtn_reader(lua_State *lua, int32_t err);
/// <summary>
/// 把 URL 查询参数压成一张 key→value 表放在栈顶,挂到哪个字段上由调用方 setfield 决定。
/// 值空(?a=)压空串而不是让键缺席——"键不存在"与"值为空"是两回事,调用方靠这个区分。
/// router 的 ctx.query 与 url.parse 的 param 都从这里取,免得空值表示分叉成两个答案
/// </summary>
/// <param name="lua">Lua 虚拟机状态</param>
/// <param name="url">已由 url_parse 填充的 url_ctx</param>
void lpub_push_url_param(lua_State *lua, url_ctx *url);
/// <summary>
/// 将 url_ctx 字段打包为 Lua 表并压栈（scheme/user/psw/host/port/path/query/segs/param）
/// </summary>
/// <param name="lua">Lua 虚拟机状态</param>
/// <param name="url">已由 url_parse 填充的 url_ctx</param>
void lpub_push_url_table(lua_State *lua, url_ctx *url);

#endif//LPUB_H_
