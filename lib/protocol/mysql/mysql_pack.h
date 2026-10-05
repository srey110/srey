#ifndef MYSQL_PACK_H_
#define MYSQL_PACK_H_

#include "protocol/mysql/mysql_bind.h"

/// <summary>
/// 构造 COM_QUIT 请求包（断开连接）。
/// </summary>
/// <param name="size">输出包大小（字节）</param>
/// <returns>请求包数据，调用方负责释放</returns>
void *mysql_pack_quit(size_t *size);
/// <summary>
/// 构造 COM_INIT_DB 请求包（切换数据库）
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="database">目标数据库名</param>
/// <param name="size">输出包大小（字节）</param>
/// <returns>请求包数据，调用方负责释放；库名超过 pending_db 容量(63 字节)时返回 NULL 且 size 置 0，
/// 一律不组包——发了包服务端会切库成功而 client.database 无从跟踪，后续 get_db 与重连握手都会用错库名</returns>
void *mysql_pack_selectdb(mysql_ctx *mysql, const char *database, size_t *size);
/// <summary>
/// 构造 COM_PING 请求包（检测连接是否存活）
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="size">输出包大小（字节）</param>
/// <returns>请求包数据，调用方负责释放</returns>
void *mysql_pack_ping(mysql_ctx *mysql, size_t *size);
/// <summary>
/// 构造 COM_QUERY 请求包（执行 SQL 语句）
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="sql">SQL 语句字符串</param>
/// <param name="mbind">查询属性参数绑定，NULL 表示无参数</param>
/// <param name="size">输出包大小（字节）</param>
/// <returns>请求包数据，调用方负责释放；SQL 长度达 INT3_MAX(16MB，MySQL 单包上限)时返回 NULL
/// 且 size 置 0，一律不组包</returns>
void *mysql_pack_query(mysql_ctx *mysql, const char *sql, mysql_bind_ctx *mbind, size_t *size);
/// <summary>
/// 同 mysql_pack_query，SQL 长度由调用方给出，不再 strlen；sql 里的 NUL 字节原样发出
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="sql">SQL 语句</param>
/// <param name="sqllen">sql 字节数</param>
/// <param name="mbind">同 mysql_pack_query</param>
/// <param name="size">输出包大小（字节）</param>
/// <returns>同 mysql_pack_query</returns>
void *mysql_pack_query2(mysql_ctx *mysql, const char *sql, size_t sqllen, mysql_bind_ctx *mbind, size_t *size);
/// <summary>
/// 构造 COM_STMT_PREPARE 请求包（预处理语句准备）
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="sql">SQL 语句字符串</param>
/// <param name="size">输出包大小（字节）</param>
/// <returns>请求包数据，调用方负责释放</returns>
void *mysql_pack_stmt_prepare(mysql_ctx *mysql, const char *sql, size_t *size);
/// <summary>
/// 同 mysql_pack_stmt_prepare，SQL 长度由调用方给出，不再 strlen
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="sql">SQL 语句</param>
/// <param name="sqllen">sql 字节数</param>
/// <param name="size">输出包大小（字节）</param>
/// <returns>同 mysql_pack_stmt_prepare</returns>
void *mysql_pack_stmt_prepare2(mysql_ctx *mysql, const char *sql, size_t sqllen, size_t *size);
/// <summary>
/// 构造 COM_STMT_EXECUTE 请求包（预处理语句执行）
/// </summary>
/// <param name="stmt">mysql_stmt_ctx</param>
/// <param name="mbind">绑定参数上下文，无参数时可为 NULL；实际绑定个数必须等于 prepare 得到的 params_count</param>
/// <param name="size">输出包大小（字节）</param>
/// <returns>请求包数据，调用方负责释放；绑定个数与 params_count 不符时返回 NULL 并把 size 置 0</returns>
void *mysql_pack_stmt_execute(mysql_stmt_ctx *stmt, mysql_bind_ctx *mbind, size_t *size);
/// <summary>
/// 构造 COM_STMT_RESET 请求包（重置预处理语句状态）
/// </summary>
/// <param name="stmt">mysql_stmt_ctx</param>
/// <param name="size">输出包大小（字节）</param>
/// <returns>请求包数据，调用方负责释放</returns>
void *mysql_pack_stmt_reset(mysql_stmt_ctx *stmt, size_t *size);
/// <summary>
/// 构造 COM_STMT_CLOSE 请求包。只组包，stmt 仍然有效，由调用方另行 mysql_stmt_free
/// </summary>
/// <param name="stmt">mysql_stmt_ctx</param>
/// <param name="size">输出包大小（字节）</param>
/// <returns>请求包数据，调用方负责释放</returns>
void *mysql_pack_stmt_close(mysql_stmt_ctx *stmt, size_t *size);
/// <summary>
/// 只释放语句的本地资源，不组包、不发送、不触碰连接状态（尤其是包序号 id）。
/// 用于"拿不到连接的串行化执行权"的场合：服务端那份语句会随连接关闭一并回收
/// </summary>
/// <param name="stmt">mysql_stmt_ctx，调用后失效</param>
void mysql_stmt_free(mysql_stmt_ctx *stmt);

#endif//MYSQL_PACK_H_
