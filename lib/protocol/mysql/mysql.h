#ifndef MYSQL_H_
#define MYSQL_H_

#include "event/evpub.h"
#include "protocol/mysql/mysql_struct.h"

// 内部函数：初始化握手回调函数指针
void _mysql_init(void *hspush);
// 内部函数：释放 mpack_ctx 数据包
void _mysql_pkfree(void *pack);
// 内部函数：释放用户数据上下文中的 MySQL 资源
void _mysql_udfree(ud_cxt *ud);
// 内部函数：SSL 握手完成后发送认证响应，成功返 PROT_SSL_OWNED。ssl 形参只为与 pgsql 同签名、共用 prots 的挂钩，本协议不做通道绑定
int32_t _mysql_ssl_exchanged(ev_ctx *ev, ud_cxt *ud, void *ssl);
// 内部函数：SSLRequest / 认证响应里发的字符集 ID。utf8mb4 连 MySQL 8.0+(握手版本串主版本 >= 8 且不含 "MariaDB")
// 发 255(utf8mb4_0900_ai_ci，8.0 起的默认排序规则)，其余照 client.charset
uint8_t _mysql_wire_charset(const mysql_ctx *mysql);
/// <summary>
/// MySQL 数据解包：验证阶段与命令阶段的统一入口。不拼 16MB 续传包：单个包载荷达到 0xFFFFFF
/// (如超过 16MB-1 的行或列定义)即置 PROT_ERROR 断连
/// </summary>
/// <param name="ev">事件上下文</param>
/// <param name="sk">连接标识，本协议不用</param>
/// <param name="client">是否客户端连接，本协议不用</param>
/// <param name="buf">接收缓冲区</param>
/// <param name="ud">连接上下文，内部维护解析状态</param>
/// <param name="size">输出：交出包时为这个响应从首包起累计的线上字节（含每包 4 字节包头），
///   只用于内存记账，不是 data 处可读的长度；认证阶段不计。没返回包时不写。必须非 NULL</param>
/// <param name="status">输出：解包状态标志，见 prot_status</param>
/// <returns>解析完成的 mpack_ctx，数据不足或出错返回 NULL</returns>
void *mysql_unpack(ev_ctx *ev, sock_ctx *sk, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);
/// <summary>
/// 查询该响应包之后是否还有更多结果集（多语句 / 存储过程 CALL 多结果集）
/// </summary>
/// <param name="mpack">mpack_ctx；NULL 视为无更多</param>
/// <returns>1=其后还有结果集；0=已是最后一个或 mpack 无效</returns>
int32_t mysql_more(mpack_ctx *mpack);
/// <summary>
/// MySQL 参数初始化。不配 evssl 时，caching_sha2_password 账号遇到服务端缓存未命中(如服务端重启后首连)
/// 须做完整认证，而明文连接上本实现不做 RSA 公钥交换(公钥无从核实，中间人换掉它就能拿到明文口令)，
/// 认证失败并打 ERROR 日志。出路：配 SSL、先经 SSL 登录一次让服务端缓存口令，或换 mysql_native_password 账号(MySQL 8.4 默认关、9.0 已移除该插件)。
/// 服务端默认认证插件不是 caching_sha2_password / mysql_native_password 时按 caching_sha2_password 作答，
/// 由服务端 AuthSwitch 到账号所用插件，切换目标仍不是这两种则认证失败
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="ip">服务器 IP 地址</param>
/// <param name="port">端口号，0 表示使用默认端口 3306</param>
/// <param name="evssl">SSL 上下文，非 NULL 时启用 SSL 连接</param>
/// <param name="user">登录用户名，最长 MYSQL_USER_LENS(128) 字节</param>
/// <param name="password">登录密码，最长 MYSQL_PASSWORD_LENS(256) 字节</param>
/// <param name="database">默认数据库名，最长 MYSQL_DB_LENS(256) 字节；NULL 或空字符串表示不设置</param>
/// <param name="charset">字符集名称（如 "utf8mb4"），不分大小写；不认识的名称打一行警告并按 0 发给服务端。
/// "utf8mb4" 连 MySQL 8.0 及以上时按 utf8mb4_0900_ai_ci(255) 发，连 5.7 / MariaDB 时按 utf8mb4_general_ci(45) 发</param>
/// <param name="maxpk">最大数据包大小，0 表示使用默认值 ONEK * ONEK</param>
/// <returns>ERR_OK 成功；ERR_FAILED 为 ip、user、password、database 之一超长。失败时 mysql 里可能已填了
/// 部分字段(含密码)，不得再用：堆上的按 SECURE_FREE 整块擦除释放</returns>
int32_t mysql_init(mysql_ctx *mysql, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, const char *database, const char *charset, uint32_t maxpk);
/// <summary>
/// 获取服务器版本信息
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <returns>服务器版本字符串</returns>
const char *mysql_version(mysql_ctx *mysql);
// 连接级的错误、自增 ID、影响行数(mysql_erro / mysql_last_id / mysql_affected_rows)只由 mysql_record 写：
// 网络线程解析时只把它们记进各自的 mpack，持该连接串行化执行权、收应答的一方再经 mysql_record 落到连接上
// (coro_utils 的命令接口收到应答即记，在交给结果回调之前)。多协程共用一条连接时，须在命令返回后、
// 本协程下次挂起之前读，让出之后别的协程可能已在同一连接上把它们改掉。握手阶段的 ERR 由建连方经 mysql_record_handshake 记

/// <summary>
/// 获取最近一个经 mysql_record 记下的 ERR 包带的错误，读取时机见上方块注释。成功的命令不清它，
/// 要按命令区分须读后调 mysql_erro_clear
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="code">输出错误码(0~65535，按线上无符号读)，NULL 表示不获取；没有错误或 ERR 包截断时为 0</param>
/// <returns>错误信息字符串，没有时为空串，超过 255 字节截断；指向 mysql 内部缓冲，
/// 下一次记下 ERR 包或 mysql_erro_clear 会改写它</returns>
const char *mysql_erro(mysql_ctx *mysql, int32_t *code);
/// <summary>
/// 清除错误信息
/// </summary>
/// <param name="mysql">mysql_ctx</param>
void mysql_erro_clear(mysql_ctx *mysql);
/// <summary>
/// 获取最近一个经 mysql_record 记下的 OK 包带的自增 ID，读取时机见上方块注释。任何记下的 OK 应答都会覆盖它
/// (非 INSERT 语句时服务端给 0)，ERR 包与结果集不改它
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <returns>自增 ID 值，按线上的无符号 64 位原样给出</returns>
uint64_t mysql_last_id(mysql_ctx *mysql);
/// <summary>
/// 获取最近一个经 mysql_record 记下的 OK 包带的影响行数，读取时机见上方块注释。ERR 包与结果集不改它，
/// 命令失败后读到的是更早那条的值
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <returns>影响的行数，按线上的无符号 64 位原样给出</returns>
uint64_t mysql_affected_rows(mysql_ctx *mysql);
/// <summary>
/// 把 OK 包的影响行数与自增 ID、或 ERR 包的错误记到连接上；其余类型的包不改它们。
/// 只在持该连接串行化执行权、收到该包的那一方调用，网络线程不调
/// </summary>
/// <param name="mysql">mysql_ctx，须是收到 mpack 的那条连接</param>
/// <param name="mpack">mpack_ctx；NULL 时什么都不做</param>
void mysql_record(mysql_ctx *mysql, mpack_ctx *mpack);
/// <summary>
/// 把握手阶段收到的 ERR 包体(握手失败时随 MSG_TYPE_HANDSHAKED 交出的载荷)解析后记到连接上，同 mysql_record 的 ERR 分支；
/// 只在持该连接串行化执行权的建连方调用
/// </summary>
/// <param name="mysql">mysql_ctx，须是收到该载荷的那条连接</param>
/// <param name="body">握手失败载荷；NULL 时什么都不做</param>
/// <param name="lens">载荷字节数</param>
/// <returns>记下的错误串(同 mysql_erro 的返回值)；body 为空时为空串</returns>
const char *mysql_record_handshake(mysql_ctx *mysql, const char *body, size_t lens);
/// <summary>
/// 取 OK 包带的影响行数，多结果集时每条语句各自一份
/// </summary>
/// <param name="mpack">mpack_ctx</param>
/// <returns>影响的行数，按线上的无符号 64 位原样给出；不是 MPACK_OK 时为 0</returns>
uint64_t mysql_pack_affected_rows(mpack_ctx *mpack);
/// <summary>
/// 取 OK 包带的自增 ID，多结果集时每条语句各自一份
/// </summary>
/// <param name="mpack">mpack_ctx</param>
/// <returns>自增 ID，按线上的无符号 64 位原样给出；非 INSERT 语句或不是 MPACK_OK 时为 0</returns>
uint64_t mysql_pack_last_id(mpack_ctx *mpack);
/// <summary>
/// 取 ERR 包带的错误
/// </summary>
/// <param name="mpack">mpack_ctx</param>
/// <param name="code">输出错误码(0~65535)，NULL 表示不获取；不是 MPACK_ERR 或 ERR 包截断时为 0</param>
/// <returns>错误信息，没有时(含不是 MPACK_ERR)为空串；指向 mpack 内部，随 mpack 失效</returns>
const char *mysql_pack_erro(mpack_ctx *mpack, int32_t *code);

#endif//MYSQL_H_
