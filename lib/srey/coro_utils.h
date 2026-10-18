#ifndef CORO_UTILS_H_
#define CORO_UTILS_H_

#include "srey/spub.h"
#include "protocol/mysql/mysql.h"
#include "protocol/pgsql/pgsql.h"
#include "protocol/mongo/mongo.h"
#include "protocol/smtp/smtp.h"
#include "protocol/kcp/kcp.h"
#include "protocol/websock.h"

// 结果集回调：一次 query / stmt_execute 可能产生多个结果集(多语句、CALL),库内部逐个回调。
// mpack 只在本次回调内有效——下一个结果集的续读会让它失效,要留数据请就地取走
// (mysql_reader_init 会把解析结果的所有权转移给调用方,拿走后不受此限)。
// 返回 ERR_FAILED 不中断循环,剩余包仍会读完;它只决定 mysql_query / mysql_stmt_execute
// 最终报成功还是失败。回调跑在连接的串行化临界区内,允许在其中调 mysql_quit 销毁本连接。
// 交给回调前本包已 mysql_record;逐条取计数与错误用 mysql_pack_*,连接级 getter 只留最后一个 OK / ERR 包的
typedef int32_t (*mysql_result_cb)(mpack_ctx *mpack, void *udata);

/// <summary>
/// dns域名解析：先 UDP 查询，失败时回退到 TCP 查询。UDP 应答须来自 DNS 服务器 IP 的 53 端口，
/// 来源不符按失败处理、回退 TCP（同 TC 置位）
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="domain">域名</param>
/// <param name="ipv6">1 ipv6 0 ipv4</param>
/// <param name="udp">1 优先使用udp查询，0 只使用tcp</param>
/// <param name="cnt">ip数量</param>
/// <returns>dns_ip 需要FREE，返回非 NULL 时 cnt 恒 &gt;= 1；解析失败或该域名没有对应记录时返回 NULL 并置 cnt 为 0。
/// 服务端明确答复"无此记录"（NOERROR/NODATA，如只有 AAAA 记录的名字查 A）或"名字不存在"（NXDOMAIN）时不再回退 TCP，直接返 NULL</returns>
struct dns_ip *dns_lookup(task_ctx *task, const char *domain, int32_t ipv6, int32_t udp, size_t *cnt);
/// <summary>
/// websocket链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="evssl">evssl_ctx</param>
/// <param name="ws">ws://host:port</param>
/// <param name="secprot">Sec-WebSocket-Protocol。协商到 "mqtt" 时本函数只完成 WS 握手，
/// 客户端方向的 mqtt_ctx 须由调用方在返回后经 mqtt_ws_bind 注入，约束见 prots_wrap.h</param>
/// <param name="netev">task_netev</param>
/// <param name="sk">out 连接标识，仅返回 ERR_OK 时有效</param>
/// <param name="spctx">out 协商到的子协议(ws_secprots_ctx)，可为 NULL 忽略；NULL 表示未协商(降级纯 WS)。由消息系统持有，仅本协程下次挂起前有效，勿持有勿释放</param>
/// <returns>ERR_OK 成功</returns>
int32_t wbsock_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ws, const char *secprot,
    int32_t netev, sock_ctx *sk, struct ws_secprots_ctx **spctx);
/// <summary>
/// redis链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="evssl">evssl_ctx</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="key">密码</param>
/// <param name="netev">task_netev</param>
/// <param name="sk">out 连接标识，仅返回 ERR_OK 时有效</param>
/// <returns>ERR_OK 成功</returns>
int32_t redis_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ip, uint16_t port,
    const char *key, int32_t netev, sock_ctx *sk);
/// <summary>
/// mysql链接。多协程共用一个 ctx 时幂等：排队等锁期间已有人把连接建好，就不再建第二条。
/// 旧连接还没关完（如命令超时后紧接着重连）时先关掉它、等到关闭确认再建新的
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="mysql">mysql_ctx, mysql_init</param>
/// <returns>ERR_OK 成功（含"连接已由别的协程建好"这一档）；失败时若命令串行化执行器是本次
/// 建链新建的，已就地回收，调用方直接返回即可，无须为了释放它补调 mysql_quit</returns>
int32_t mysql_connect(task_ctx *task, mysql_ctx *mysql);
/// <summary>
/// 选择数据库
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="database">数据库</param>
/// <returns>ERR_OK 成功；库名超 MYSQL_DB_LENS(256) 字节时不发包直接返 ERR_FAILED</returns>
int32_t mysql_selectdb(mysql_ctx *mysql, const char *database);
/// <summary>
/// ping
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t mysql_ping(mysql_ctx *mysql);
/// <summary>
/// 执行SQL语句。服务端支持 CLIENT_SESSION_TRACK（MySQL 5.7+）时 "USE xxx" 会经 OK 包的
/// session-state-change 回带并更新 client.database；老服务端不带该信息，那时切库须用
/// mysql_selectdb，否则重连会按旧库名握手。
/// 接收侧不拼 16MB 续传包：单个包载荷达 16MB-1（0xFFFFFF，如超大的行）即按协议错误断连，返回 ERR_FAILED。
/// 本组 mysql 命令接口收到应答即 mysql_record，连接级 mysql_erro / mysql_last_id / mysql_affected_rows
/// 须在命令返回后、本协程下次挂起之前读（见 mysql.h 里它们上方的块注释）
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="sql">SQL语句</param>
/// <param name="mbind">mysql_bind_ctx</param>
/// <param name="cb">结果集回调；NULL 表示只把结果集读完不回调（INSERT/UPDATE 这类）</param>
/// <param name="udata">透传给 cb</param>
/// <returns>ERR_OK 全部结果集读完且回调都成功；ERR_FAILED 组包失败、网络失败、
/// 未持锁(不在协程内或连接正在销毁)、任一回调返回失败、或 cb 为 NULL 时服务端回了 ERR 应答
/// （有 cb 时 ERR 应答不算失败：ERR 包照样交给 cb，成败由 cb 说）</returns>
int32_t mysql_query(mysql_ctx *mysql, const char *sql, mysql_bind_ctx *mbind,
                    mysql_result_cb cb, void *udata);
/// <summary>
/// 预处理
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="sql">SQL语句</param>
/// <returns>mysql_stmt_ctx NULL 失败</returns>
mysql_stmt_ctx *mysql_stmt_prepare(mysql_ctx *mysql, const char *sql);
/// <summary>
/// 预处理执行。接收侧 16MB 续传包的限制同 mysql_query
/// </summary>
/// <param name="stmt">mysql_stmt_ctx</param>
/// <param name="mbind">mysql_bind_ctx</param>
/// <param name="cb">结果集回调；NULL 的含义同 mysql_query</param>
/// <param name="udata">透传给 cb</param>
/// <returns>ERR_OK 全部结果集读完且回调都成功；失败含义同 mysql_query。
/// 连接已关闭、或中途重连过（stmt_id 属于旧连接）时不发包直接返回 ERR_FAILED，
/// 此时须重新 mysql_stmt_prepare</returns>
int32_t mysql_stmt_execute(mysql_stmt_ctx *stmt, mysql_bind_ctx *mbind,
                           mysql_result_cb cb, void *udata);
/// <summary>
/// 预处理重置
/// </summary>
/// <param name="stmt">mysql_stmt_ctx</param>
/// <returns>ERR_OK 成功；连接已关闭或中途重连过时的行为同 mysql_stmt_execute</returns>
int32_t mysql_stmt_reset(mysql_stmt_ctx *stmt);
/// <summary>
/// 关闭预处理语句并释放相关资源。拿不到该连接的串行化执行权时（不在协程内、连接正在销毁，
/// 或这条连接不受本套 API 管——未经 mysql_connect 建链）只做本地释放、不发 COM_STMT_CLOSE，
/// 服务端那份语句随连接关闭一并回收
/// </summary>
/// <param name="stmt">mysql_stmt_ctx，调用后失效</param>
void mysql_stmt_close(mysql_stmt_ctx *stmt);
/// <summary>
/// 关闭链接，并回收该连接的串行化执行器（排队中的命令被唤醒并失败返回）。
/// 先排在在途命令之后再退出，不把别人半途的等待打断。**须在协程内调用**（内部要等断连确认）。
/// 另一个协程已在销毁同一条连接时本次直接返回，善后归先到的那一方。
/// 没有执行器的连接（未经 *_connect 建链）不排队直接断连——那种连接上的命令本就不串行。
/// smtp_quit / pgsql_quit / mongo_quit 同此契约
/// </summary>
/// <param name="mysql">mysql_ctx</param>
void mysql_quit(mysql_ctx *mysql);
// 以下 smtp 接口经连接内的串行化执行器串行：一封邮件是 MAIL FROM → N×RCPT TO → DATA →
// 正文一长串往返，两个协程同时发信会把收件人混到一起。因此 ping / send 多一种失败：
// 调用方不在协程内、或该连接正在 smtp_quit 销毁
/// <summary>
/// 电子邮件建立链接；幂等与旧连接的处置同 mysql_connect
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="smtp">smtp_ctx</param>
/// <returns>ERR_OK 成功；幂等性与失败清理契约同 mysql_connect</returns>
int32_t smtp_connect(task_ctx *task, smtp_ctx *smtp);
/// <summary>
/// 发送 QUIT 并关闭链接；排队、协程、无执行器的契约同 mysql_quit
/// </summary>
/// <param name="smtp">smtp_ctx</param>
void smtp_quit(smtp_ctx *smtp);
/// <summary>
/// ping测试
/// </summary>
/// <param name="smtp">smtp_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t smtp_ping(smtp_ctx *smtp);
/// <summary>
/// 邮件发送。先在锁外 mail_pack 组好整封信，组不出来（取不到熵）就不发任何命令、直接失败，连接不动。
/// 锁覆盖整封邮件（含失败时补发的 RSET），期间其他协程的投递排队等待。
/// 投递成功不发 RSET（正文收到 250 时服务端已清空事务）；投递失败才发 RSET 清掉服务端留下的半截信封，
/// RSET 也失败即关闭连接——那说明连接已不干净，留着它下一封信只会被回 503
/// </summary>
/// <param name="smtp">smtp_ctx</param>
/// <param name="mail">mail_ctx</param>
/// <returns>ERR_OK 邮件已投递。只反映这封邮件的成败，不反映连接状态——
/// 投递失败且补发的 RSET 也失败时连接已被关掉，
/// 下一次发送要么先 smtp_ping 重连，要么就在已关的连接上失败</returns>
int32_t smtp_send(smtp_ctx *smtp, mail_ctx *mail);
// 以下 pgsql 命令接口全部经连接内的串行化执行器串行发出：pgsql 一条命令要读到 ReadyForQuery
// 才算完，copy_in 更是要收两包，多协程共用一条连接时命令交错会让整条连接报错。
// 因此每个命令都多一种失败：调用方不在协程内、或该连接正在 pgsql_quit 销毁（失败值同各自的
// 网络失败，不额外区分）。经 pgsql_try_connect 自行建立的连接不受管，不串行化。
// 唯一有意不串行化的是 pgsql_cancel——见该函数说明
/// <summary>
/// pgsql链接；幂等与旧连接的处置同 mysql_connect
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="pg">pgsql_ctx, pgsql_init</param>
/// <returns>ERR_OK 成功；幂等性与失败清理契约同 mysql_connect</returns>
int32_t pgsql_connect(task_ctx *task, pgsql_ctx *pg);
/// <summary>
/// 在独立 TCP 连接上向服务端发送 CancelRequest，中止当前正在执行的查询。
/// 服务端处理后主动关闭该连接、无响应；原连接会收到错误回包。
/// 有意不参与命令串行化（排队等锁就等到查询自己结束了），也不往原连接写任何字节
/// </summary>
/// <param name="pg">pgsql_ctx 指针，须已成功连接（pid/key 已初始化）</param>
/// <returns>ERR_OK 发送成功，ERR_FAILED 连接未建立或网络失败</returns>
int32_t pgsql_cancel(pgsql_ctx *pg);
/// <summary>
/// 发送 Terminate 并关闭链接；排队、协程、无执行器的契约同 mysql_quit
/// </summary>
/// <param name="pg">pgsql_ctx</param>
void pgsql_quit(pgsql_ctx *pg);
/// <summary>
/// 选择数据库（断开重连，库名在重连握手时生效）
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="database">数据库</param>
/// <returns>ERR_OK 成功；库名超 63 字节时不断连直接返 ERR_FAILED，原连接与原库名均保持不变</returns>
int32_t pgsql_selectdb(pgsql_ctx *pg, const char *database);
/// <summary>
/// ping
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t pgsql_ping(pgsql_ctx *pg);
/// <summary>
/// 执行SQL语句。多语句串出错时的语义见 pgsql_result_count：串内含 COMMIT 等显式事务控制时，
/// 出错前已提交的结果拿不到，不能据失败判定全部回滚
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="sql">SQL语句</param>
/// <returns>NULL 失败（含 SQL 使消息体超 INT32_MAX、组包失败：此时不发包，连接不动）；
/// pgpack_ctx 归消息系统所有，只在本协程下次挂起前有效，勿释放</returns>
pgpack_ctx *pgsql_query(pgsql_ctx *pg, const char *sql);
/// <summary>
/// 预处理
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="name">名称</param>
/// <param name="sql">sql语句</param>
/// <param name="nparam">参数数量</param>
/// <param name="oids">参数OID(pgsql_macro.h)</param>
/// <returns>ERR_OK 成功；组包失败（消息体超 INT32_MAX）时不发包、连接不动，返回 ERR_FAILED</returns>
int32_t pgsql_stmt_prepare(pgsql_ctx *pg, const char *name, const char *sql, int16_t nparam, uint32_t *oids);
/// <summary>
/// 预处理执行
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="name">名称</param>
/// <param name="bind">pgsql_bind_ctx；实际绑定个数须等于 pgsql_bind_init 声明的 nparam，不符即失败。
/// 与预处理语句声明的个数是否相符不在本地校验，见 pgsql_pack_stmt_execute</param>
/// <param name="resultformat">pgpack_format</param>
/// <returns>NULL 失败；pgpack_ctx 的所有权与有效期同 pgsql_query</returns>
pgpack_ctx *pgsql_stmt_execute(pgsql_ctx *pg, const char *name, pgsql_bind_ctx *bind, pgpack_format resultformat);
/// <summary>
/// 预处理关闭
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="name">名称</param>
void pgsql_stmt_close(pgsql_ctx *pg, const char *name);
/// <summary>
/// 执行 COPY FROM STDIN（单次批量写入）
/// 内部流程：COPY SQL、CopyData、CopyDone 一次发出 → 等 CopyInResponse → 等 ReadyForQuery。
/// 语句本身失败时数据也已发出（服务端忽略），数据量大时白传一次
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="sql">包含 FROM STDIN 的 COPY SQL 语句</param>
/// <param name="data">要写入的原始数据</param>
/// <param name="lens">数据字节数</param>
/// <returns>NULL 失败（组包失败时不发包、连接不动，同 pgsql_query），pgpack_ctx（PGPACK_OK / PGPACK_ERR），
/// 所有权与有效期同 pgsql_query</returns>
pgpack_ctx *pgsql_copy_in(pgsql_ctx *pg, const char *sql, const void *data, size_t lens);
/// <summary>
/// 执行 COPY TO STDOUT，返回服务端输出的全部数据
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="sql">包含 TO STDOUT 的 COPY SQL 语句</param>
/// <returns>NULL 失败，pgpack_ctx（PGPACK_COPY_OUT / PGPACK_ERR），pack 字段为 pgpack_copy_out_ctx*；
/// 所有权与有效期同 pgsql_query</returns>
pgpack_ctx *pgsql_copy_out(pgsql_ctx *pg, const char *sql);
// 以下 mongo 命令接口全部经连接内的串行化执行器串行发出（含 MORETOCOME 的只发不等——
// 不等响应也不能乱序，后面那条 find 得看得见前面这批 insert）。因此每个命令都多一种失败：
// 调用方不在协程内、或该连接正在 mongo_quit 销毁（失败值同各自的网络失败，不额外区分）。
// 经 mongo_try_connect 自行建立的连接不受管，不串行化。
// 例外：mongo_startsession（本地生成 lsid）与 mongo_begin（只改本地状态）不发命令、不取锁、不挂起。
// 串行化只保证单条命令原子、不保证事务原子，见 mongo_begin
/// <summary>
/// 链接 mongodb：建连 + hello + 认证整段在锁内完成，返回成功即可直接发命令。
/// 认证仅在 mongo_user_pwd 设过用户名时进行，算法取 mongo_authmod（默认 SCRAM-SHA-256）；
/// 两个 setter 都须在本函数之前调用，ping 重连也复用它们。服务端最低 4.4，理由见 mongo_init。
/// 幂等与旧连接的处置同 mysql_connect
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="mongo">mongo_ctx</param>
/// <returns>ERR_OK 成功；hello 或认证失败时本函数已关闭 socket，
///   幂等性与其余清理契约同 mysql_connect</returns>
int32_t mongo_connect(task_ctx *task, mongo_ctx *mongo);
/// <summary>
/// 关闭链接（mongo 没有退出命令，断连就是退出）；排队、协程、无执行器的契约同 mysql_quit
/// </summary>
/// <param name="mongo">mongo_ctx</param>
void mongo_quit(mongo_ctx *mongo);
/// <summary>
/// hello 命令 显示该节点在副本集中的角色信息，包括是否为主副本。
/// mongo_connect 内部已发过一次不带 options 的 hello，此处仅用于需要额外带 options 的场景，
/// 且不会在 ping 重连时重放。恒不带事务上下文（见 mongo_pack_hello）。
/// 成功时把回包的 logicalSessionTimeoutMinutes 记进连接，供 mongo_startsession 用
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="options">可选 其他参数 document (saslSupportedMechs)</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <returns>NULL 失败；mgopack_ctx 归消息系统所有，只在本协程下次挂起前有效，勿释放</returns>
mgopack_ctx *mongo_hello(mongo_ctx *mongo, char *options, size_t optlens);
/// <summary>
/// ping 命令
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t mongo_ping(mongo_ctx *mongo);
/// <summary>
/// drop 命令 删除当前集合 MORETOCOME 可用
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="options">可选 其他参数 document (writeConcern comment)</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <returns>ERR_OK 成功</returns>
int32_t mongo_drop(mongo_ctx *mongo, char *options, size_t optlens);
/// <summary>
/// insert 命令 插入一个或多个文档 MORETOCOME 可用
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="docs">[ document, ... ]</param>
/// <param name="dlens">docs长度</param>
/// <param name="options">可选 其他参数 document (ordered maxTimeMS writeConcern bypassDocumentValidation comment)</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <returns>ERR_FAILED 失败  其他 插入的数量；置 MORETOCOME 时不等应答，成功恒返回 0（不是条数）</returns>
int32_t mongo_insert(mongo_ctx *mongo, char *docs, size_t dlens, char *options, size_t optlens);
/// <summary>
/// update 命令 更新一个或多个文档 MORETOCOME 可用
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="updates">[{q:u:...}, ...]</param>
/// <param name="ulens">updates长度</param>
/// <param name="options">可选 其他参数 document (ordered maxTimeMS writeConcern bypassDocumentValidation comment let)</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <returns>ERR_FAILED 失败  其他 应答里的 n,即匹配到的文档数(matched);不是 nModified,值未变化的 $set 同样计入；
/// 置 MORETOCOME 时成功恒返回 0，同 mongo_insert</returns>
int32_t mongo_update(mongo_ctx *mongo, char *updates, size_t ulens, char *options, size_t optlens);
/// <summary>
/// delete 命令 删除一个或多个文档 MORETOCOME 可用
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="deletes">[{q:...}, ...]</param>
/// <param name="dlens">deletes长度</param>
/// <param name="options">可选 其他参数 document (comment let ordered writeConcern maxTimeMS)</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <returns>ERR_FAILED 失败  其他 删除的数量；置 MORETOCOME 时成功恒返回 0，同 mongo_insert</returns>
int32_t mongo_delete(mongo_ctx *mongo, char *deletes, size_t dlens, char *options, size_t optlens);
/// <summary>
/// bulkwrite 命令 在一个请求中对多个集合执行多次插入、更新和删除操作  MORETOCOME 可用
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="ops">[insert,update,delete...]</param>
/// <param name="olens">ops长度</param>
/// <param name="nsinfo">[ns...] 操作的命名空间（数据库和集合）.将ops中每个操作的命名空间ID索引设置为ns中匹配的命名空间大量索引.索引从0开始</param>
/// <param name="nlens">nsinfo长度</param>
/// <param name="options">可选 其他参数 document (ordered bypassDocumentValidation comment let errorsOnly cursor writeConcern)</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <returns>设置MORETOCOME始终返回NULL, 未设置则 NULL 失败；mgopack_ctx 的所有权与有效期同 mongo_hello</returns>
mgopack_ctx *mongo_bulkwrite(mongo_ctx *mongo, char *ops, size_t olens, char *nsinfo, size_t nlens, char *options, size_t optlens);
/// <summary>
/// find 命令 选择集合或视图中的文档
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="filter">可选 查询谓词 document</param>
/// <param name="flens">filter长度</param>
/// <param name="options">可选 其他参数 document 
/// (sort projection hint skip limit batchSize singleBatch comment maxTimeMS readConcern max min returnKey
/// showRecordId tailable oplogReplay noCursorTimeout awaitData allowPartialResults collation allowDiskUse let) 
/// </param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <returns>NULL 失败；mgopack_ctx 的所有权与有效期同 mongo_hello</returns>
mgopack_ctx *mongo_find(mongo_ctx *mongo, char *filter, size_t flens, char *options, size_t optlens);
/// <summary>
/// aggregate 命令 聚合
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="pipeline">[ stage, ... ] 聚合管道阶段数组</param>
/// <param name="pllens">pipeline长度</param>
/// <param name="options">可选 其他参数 document 
/// (explain allowDiskUse maxTimeMS bypassDocumentValidation readConcern collation hint comment writeConcern let)
/// </param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <returns>NULL 失败；mgopack_ctx 的所有权与有效期同 mongo_hello</returns>
mgopack_ctx *mongo_aggregate(mongo_ctx *mongo, char *pipeline, size_t pllens, char *options, size_t optlens);
/// <summary>
/// getMore 命令 返回游标当前指向的文档的后续批次
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="cursorid">游标标识符</param>
/// <param name="options">可选 其他参数 document (batchSize maxTimeMS comment)；集合固定取 mongo_collection 设的当前集合，别在这里再带 collection</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <returns>NULL 失败；mgopack_ctx 的所有权与有效期同 mongo_hello</returns>
mgopack_ctx *mongo_getmore(mongo_ctx *mongo, int64_t cursorid, char *options, size_t optlens);
/// <summary>
/// killCursors 命令 终止集合的一个或多个指定游标  MORETOCOME 可用
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="cursorids">游标标识符 [cursorid, ...]</param>
/// <param name="cslens">cursorids长度</param>
/// <param name="options">可选 其他参数 document (comment)</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <returns>设置MORETOCOME始终返回NULL, 未设置则 NULL 失败；mgopack_ctx 的所有权与有效期同 mongo_hello</returns>
mgopack_ctx *mongo_killcursors(mongo_ctx *mongo, char *cursorids, size_t cslens, char *options, size_t optlens);
/// <summary>
/// distinct 命令 查找单个集合中指定字段的不同值
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="key">字段</param>
/// <param name="query">可选 查询 document </param>
/// <param name="qlens">query长度</param>
/// <param name="options">可选 其他参数 document (readConcern collation comment hint)</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <returns>NULL 失败；mgopack_ctx 的所有权与有效期同 mongo_hello</returns>
mgopack_ctx *mongo_distinct(mongo_ctx *mongo, const char *key, char *query, size_t qlens, char *options, size_t optlens);
/// <summary>
/// findandmodify 命令 返回并修改单个文档
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="query">可选 条件 document</param>
/// <param name="qlens">query长度</param>
/// <param name="remove">true:删除所选文档 false:更新所选文档,必须有update</param>
/// <param name="pipeline">update是否为pipeline</param>
/// <param name="update">更新所选文档 document</param>
/// <param name="ulens">update长度</param>
/// <param name="options">可选 其他参数 document
/// (sort new fields upsert bypassDocumentValidation writeConcern maxTimeMS collation arrayFilters hint comment let)
/// </param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <returns>NULL 失败；mgopack_ctx 的所有权与有效期同 mongo_hello</returns>
mgopack_ctx *mongo_findandmodify(mongo_ctx *mongo, char *query, size_t qlens,
    int32_t remove, int32_t pipeline, char *update, size_t ulens, char *options, size_t optlens);
/// <summary>
/// count 命令 计算集合或视图中的文档数量。不参与事务：连接绑着事务时也按事务外执行（见 mongo_pack_count），
/// 事务内计数用 mongo_aggregate 的 $count 阶段
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="query">可选 查询，选择哪些文档要在集合或视图中计数</param>
/// <param name="qlens">query长度</param>
/// <param name="options">可选 其他参数 document (limit skip hint readConcern maxTimeMS collation comment)</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <returns>ERR_FAILED 失败  其他 文档数量；计数超出 int32 时同样返回 ERR_FAILED</returns>
int32_t mongo_count(mongo_ctx *mongo, char *query, size_t qlens, char *options, size_t optlens);
/// <summary>
/// createindexes 命令 为集合构建一个或多个索引  MORETOCOME 可用
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="indexes">指定要创建的索引[{key:{...},name:}...]</param>
/// <param name="ilens">indexes长度</param>
/// <param name="options">可选 其他参数 document (writeConcern commitQuorum comment)</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <returns>ERR_OK 成功</returns>
int32_t mongo_createindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t optlens);
/// <summary>
/// dropindexes 命令 从集合中删除索引  MORETOCOME 可用
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="indexes">要删除的一个或多个索引 <arrayofstrings></param>
/// <param name="ilens">indexes长度</param>
/// <param name="options">可选 其他参数 document (writeConcern comment)</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <returns>ERR_OK 成功</returns>
int32_t mongo_dropindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t optlens);
/// <summary>
/// 启动新会话。lsid 在本地生成（v4 UUID），不发命令、不取锁、不挂起，服务端第一次见到该 lsid 时自动建会话，
/// 故连接有问题要到第一次使用时才暴露。超时分钟数在开会话时取连接最近一次成功的 mongo_hello
/// （建连、ping 重连、手动调用都算）回的 logicalSessionTimeoutMinutes，之后的 hello 不影响已开的会话；
/// 还没成功 hello 过、或回包没给时为 0（超时未知）。会话按 lsid 记在服务端、与连接无关，连接 quit / 重连后仍可继续用
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <returns>NULL 失败（随机源失败） mongo_session</returns>
mongo_session *mongo_startsession(mongo_ctx *mongo);
/// <summary>
/// refreshsession 命令 刷新空闲会话
/// </summary>
/// <param name="session">mongo_session</param>
/// <returns>ERR_OK 成功</returns>
int32_t mongo_refreshsession(mongo_session *session);
/// <summary>
/// endsessions 命令 使会话过期,释放mongo_session。
/// endsessions 一律发送：服务端的会话记录不随连接消失，漏发要挂到会话超时才回收。
/// 包恒带 MORETOCOME（见 mongo_pack_endsession）只发不等，结果不看
/// </summary>
/// <param name="session">mongo_session；所有权交出，返回后调用方手上的指针失效。
///   释放排在内部等锁的那次挂起之后：挂起窗口里 session 仍然有效，在途的
///   commit/rollback 由 serial 锁排在本函数之前，醒来还要读 session->options。
///   调用方不得在此期间另行释放，返回后也不得再碰</param>
void mongo_freesession(mongo_session *session);
/// <summary>
/// 事务开始。一条连接同时只允许一个活跃事务（CRUD 命令的事务上下文取自连接上的当前绑定），
/// 同一 session 重复调用视为开新事务（递增 txnNumber）。
/// 串行化只保证单条命令原子、不保证事务原子：别人的命令挤在 begin 与 commit/rollback 之间时，
/// 组包侧照样给它附上本事务的 lsid/txnNumber；要隔离须在 begin..commit 外自己套一层 coro_serial，
/// 或给事务用独占连接。
/// 事务进行中连接换过（断开后重连）即作废（mongo_session_stale）：此后带事务上下文的命令一律组包失败，
/// 组包后排队等锁期间才换的，拿到锁后同样不发、按失败返回（见 mongo_txn_moved）。
/// 绑定留到 commit/rollback 收尾，二者都在新连接上发 abortTransaction 让服务端当场中止、放锁；要继续须重新 begin
/// </summary>
/// <param name="session">mongo_session</param>
/// <returns>ERR_OK 成功；该连接上已有别的 session 处于事务中（含已作废还没收尾的）时返回 ERR_FAILED，
/// 且不改动任何状态。重连只废掉在途事务不废会话，重连后拿旧 session 重新 begin 是正常用法</returns>
int32_t mongo_begin(mongo_session *session);
/// <summary>
/// 事务提交。begin 之后还没组出过首条命令的空事务不发包，本地解绑、释放状态并按成功返回
/// </summary>
/// <param name="session">mongo_session</param>
/// <param name="options">可选 其他参数 document (writeConcern comment)</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <returns>ERR_OK 成功。组包/网络失败时事务状态原样保留，可重试；服务端有响应即释放事务状态
/// （命令本身失败也不再可重试），等回包期间同一 session 已重新 mongo_begin 的除外，那份状态归新事务。
/// 事务已随重连作废时不能提交（空事务除外，照上面本地收尾）：改发 abortTransaction 了结它（服务端有响应即解绑、释放状态），返回 ERR_FAILED。
/// 连接已不再绑定该 session（被 mongo_clear_session 清过，或被别的 session 接管）时同样不发送、
/// 返回 ERR_FAILED，本地事务状态原样保留。两种情形要再开事务都须重新 mongo_begin（会递增 txnNumber）</returns>
int32_t mongo_commit(mongo_session *session, char *options, size_t optlens);
/// <summary>
/// 事务回滚
/// </summary>
/// <param name="session">mongo_session</param>
/// <param name="options">可选 其他参数 document (writeConcern comment)</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <returns>ERR_OK 成功。空事务、状态保留/释放的时机、连接不再绑定该 session 时的处置同 mongo_commit；
/// 事务已随重连作废照常发 abortTransaction（空事务不发），返回值即它的结果</returns>
int32_t mongo_rollback(mongo_session *session, char *options, size_t optlens);
/// <summary>
/// kcp 同步建立会话:kcp_start 后挂起当前协程,等 event 线程实际建会话完成(或 conv 冲突、fd 不是 UDP_KCP 而失败)后返回;须在协程内调用。
/// 唤醒 sess 由本函数内部生成(每次新会话一个,故 stop 后重启不会被上一会话的 CLOSE 击穿)。
/// 数据到达时推送的目标固定为调用方所在 task(即 task->handle);不等待也可用 kcp_start 异步发起并自定目标 handle,
/// 结果同样会以 MSG_TYPE_HANDSHAKED 推给 task_handshaked 注册的回调(msg.subtype 为 PACK_UDP_KCP)
/// </summary>
/// <param name="task">task_ctx,同时也是会话数据的推送目标</param>
/// <param name="kcp">已 kcp_init 的 kcp_ctx</param>
/// <param name="ip">对端 IP</param>
/// <param name="port">对端端口;0 为学习模式,同 kcp_start</param>
/// <param name="cfg">KCP 可调参数;NULL 用库默认(见 kcp_config)</param>
/// <returns>ERR_OK 会话建立成功;ERR_FAILED 失败(kcp_start 本身失败/conv 冲突或 fd 不是 UDP_KCP/超时/会话被关闭)。
/// 失败与之后 kcp_synsend 遇到 CLOSE 都只把句柄置 stopped、不清 sess,关闭回调按 sess 认会话</returns>
int32_t kcp_synstart(task_ctx *task, struct kcp_ctx *kcp,
                     const char *ip, uint16_t port, const struct kcp_config *cfg);
/// <summary>
/// kcp 同步发送并等待响应:发送后挂起当前协程,收到对端响应后返回;须在协程内调用。
/// sess 取 kcp_start 时传入的值,同一会话上的多次 synsend 按 FIFO 排队唤醒;
/// 以 kcp_start(sess=0) 异步建立的会话不能用本函数。
/// 超时会 kcp_stop 销毁该会话;被 CLOSE 唤醒(socket 关闭、对端失联)时句柄也已置停。两种情形都要重新 kcp_start 才能再用
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="kcp">已 kcp_start 的 kcp_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">数据长度</param>
/// <param name="copy">1 拷贝数据;0 转移所有权</param>
/// <param name="size">出参:响应数据长度</param>
/// <returns>响应数据(仅在下次 yield 前有效,需保留请自行拷贝);超时或失败返回 NULL</returns>
void *kcp_synsend(task_ctx *task, struct kcp_ctx *kcp, void *data, size_t lens, int32_t copy, size_t *size);

#endif//CORO_UTILS_H_
