#ifndef CORO_UTILS_H_
#define CORO_UTILS_H_

#include "srey/spub.h"
#include "protocol/mysql/mysql.h"
#include "protocol/pgsql/pgsql.h"
#include "protocol/mongo/mongo.h"
#include "protocol/smtp/smtp.h"

/// <summary>
/// dns域名解析
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="domain">域名</param>
/// <param name="ipv6">1 ipv6 0 ipv4</param>
/// <param name="cnt">ip数量</param>
/// <returns>dns_ip 需要FREE</returns>
struct dns_ip *dns_lookup(task_ctx *task, const char *domain, int32_t ipv6, size_t *cnt);
/// <summary>
/// websocket链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="evssl">evssl_ctx</param>
/// <param name="ws">ws://host:port</param>
/// <param name="secproto">Sec-WebSocket-Protocol</param>
/// <param name="skid">链接ID</param>
/// <param name="netev">task_netev</param>
/// <returns>socket句柄</returns>
SOCKET wbsock_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ws, const char *secprot, uint64_t *skid, int32_t netev);
/// <summary>
/// redis链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="evssl">evssl_ctx</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="key">密码</param>
/// <param name="skid">链接ID</param>
/// <param name="netev">task_netev</param>
/// <returns>socket句柄</returns>
SOCKET redis_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ip, uint16_t port, const char *key, uint64_t *skid, int32_t netev);
/// <summary>
/// myql链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="mysql">mysql_ctx, mysql_init</param>
/// <returns>ERR_OK 成功</returns>
int32_t mysql_connect(task_ctx *task, mysql_ctx *mysql);
/// <summary>
/// 选择数据库
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="mysql">数据库</param>
/// <returns>ERR_OK 成功</returns>
int32_t mysql_selectdb(mysql_ctx *mysql, const char *database);
/// <summary>
/// ping
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t mysql_ping(mysql_ctx *mysql);
/// <summary>
/// 执行SQL语句
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="sql">SQL语句</param>
/// <param name="mbind">mysql_bind_ctx</param>
/// <returns>mpack_ctx NULL 失败</returns>
mpack_ctx *mysql_query(mysql_ctx *mysql, const char *sql, mysql_bind_ctx *mbind);
/// <summary>
/// 预处理
/// </summary>
/// <param name="mysql">mysql_ctx</param>
/// <param name="sql">SQL语句</param>
/// <returns>mysql_stmt_ctx NULL 失败</returns>
mysql_stmt_ctx *mysql_stmt_prepare(mysql_ctx *mysql, const char *sql);
/// <summary>
/// 预处理执行
/// </summary>
/// <param name="stmt">mysql_stmt_ctx</param>
/// <param name="mbind">mysql_bind_ctx</param>
/// <returns>mpack_ctx NULL 失败</returns>
mpack_ctx *mysql_stmt_execute(mysql_stmt_ctx *stmt, mysql_bind_ctx *mbind);
/// <summary>
/// 预处理重置
/// </summary>
/// <param name="stmt">mysql_stmt_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t mysql_stmt_reset(mysql_stmt_ctx *stmt);
/// <summary>
/// 退出关闭链接
/// </summary>
/// <param name="mysql">mysql_ctx</param>
void mysql_quit(mysql_ctx *mysql);
/// <summary>
/// 电子邮件建立链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="smtp">smtp_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t smtp_connect(task_ctx *task, smtp_ctx *smtp);
/// <summary>
/// 邮件关闭
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
/// 邮件发送
/// </summary>
/// <param name="smtp">smtp_ctx</param>
/// <param name="mail">mail_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t smtp_send(smtp_ctx *smtp, mail_ctx *mail);
/// <summary>
/// pgsql链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="pg">pgsql_ctx, pgsql_init</param>
/// <returns>ERR_OK 成功</returns>
int32_t pgsql_connect(task_ctx *task, pgsql_ctx *pg);
/// <summary>
/// 关闭链接
/// </summary>
/// <param name="pg">pgsql_ctx</param>
void pgsql_quit(pgsql_ctx *pg);
/// <summary>
/// 选择数据库
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="database">数据库</param>
/// <returns>ERR_OK 成功</returns>
int32_t pgsql_selectdb(pgsql_ctx *pg, const char *database);
/// <summary>
/// ping
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t pgsql_ping(pgsql_ctx *pg);
/// <summary>
/// 执行SQL语句
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="sql">SQL语句</param>
/// <returns>NULL 失败  pgpack_ctx</returns>
pgpack_ctx *pgsql_query(pgsql_ctx *pg, const char *sql);
/// <summary>
/// 预处理
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="name">名称</param>
/// <param name="sql">sql语句</param>
/// <param name="nparam">参数数量</param>
/// <param name="oids">参数OID(pgsql_macro.h)</param>
/// <returns>ERR_OK 成功</returns>
int32_t pgsql_stmt_prepare(pgsql_ctx *pg, const char *name, const char *sql, int16_t nparam, uint32_t *oids);
/// <summary>
/// 预处理执行
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="name">名称</param>
/// <param name="bind">pgsql_bind_ctx</param>
/// <param name="resultformat">pgpack_format</param>
/// <returns>NULL 失败  pgpack_ctx</returns>
pgpack_ctx *pgsql_stmt_execute(pgsql_ctx *pg, const char *name, pgsql_bind_ctx *bind, pgpack_format resultformat);
/// <summary>
/// 预处理关闭
/// </summary>
/// <param name="pg">pgsql_ctx</param>
/// <param name="name">名称</param>
void pgsql_stmt_close(pgsql_ctx *pg, const char *name);
/// <summary>
/// 链接mongodb
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="mongo">mongo_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t mongo_connect(task_ctx *task, mongo_ctx *mongo);
/// <summary>
/// 用户验证
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="authmod">SCRAM-SHA-1 SCRAM-SHA-256</param>
/// <param name="user">用户名</param>
/// <param name="pwd">密码</param>
/// <returns>ERR_OK 成功</returns>
int32_t mongo_auth(mongo_ctx *mongo, const char *authmod, const char *user, const char *pwd);
/// <summary>
/// hello 命令 显示该节点在副本集中的角色信息，包括是否为主副本
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="options">可选 其他参数 document (saslSupportedMechs)</param>
/// <returns>NULL 失败</returns>
mgopack_ctx *mongo_hello(mongo_ctx *mongo, char *options);
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
/// <returns>ERR_OK 成功</returns>
int32_t mongo_drop(mongo_ctx *mongo, char *options);
/// <summary>
/// insert 命令 插入一个或多个文档 MORETOCOME 可用
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="docs">[ document, ... ]</param>
/// <param name="dlens">docs长度</param>
/// <param name="options">可选 其他参数 document (ordered maxTimeMS writeConcern bypassDocumentValidation comment)</param>
/// <returns>ERR_FAILED 失败  其他 插入的数量</returns>
int32_t mongo_insert(mongo_ctx *mongo, char *docs, size_t dlens, char *options);
/// <summary>
/// update 命令 更新一个或多个文档 MORETOCOME 可用
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="updates">[{q:u:...}, ...]</param>
/// <param name="ulens">updates长度</param>
/// <param name="options">可选 其他参数 document (ordered maxTimeMS writeConcern bypassDocumentValidation comment let)</param>
/// <returns>ERR_FAILED 失败  其他 更新的数量</returns>
int32_t mongo_update(mongo_ctx *mongo, char *updates, size_t ulens, char *options);
/// <summary>
/// delete 命令 删除一个或多个文档 MORETOCOME 可用
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="deletes">[{q:...}, ...]</param>
/// <param name="dlens">deletes长度</param>
/// <param name="options">可选 其他参数 document (comment let ordered writeConcern maxTimeMS)</param>
/// <returns>ERR_FAILED 失败  其他 删除的数量</returns>
int32_t mongo_delete(mongo_ctx *mongo, char *deletes, size_t dlens, char *options);
/// <summary>
/// bulkwrite 命令 在一个请求中对多个集合执行多次插入、更新和删除操作  MORETOCOME 可用
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="ops">[insert,update,delete...]</param>
/// <param name="olens">ops长度</param>
/// <param name="nsinfo">[ns...] 操作的命名空间（数据库和集合）.将ops中每个操作的命名空间ID索引设置为ns中匹配的命名空间大量索引.索引从0开始</param>
/// <param name="nlens">nsinfo长度</param>
/// <param name="options">可选 其他参数 document (ordered bypassDocumentValidation comment let errorsOnly cursor writeConcern)</param>
/// <returns>设置MORETOCOME始终返回NULL, 未设置则 NULL 失败</returns>
mgopack_ctx *mongo_bulkwrite(mongo_ctx *mongo, char *ops, size_t olens, char *nsinfo, size_t nlens, char *options);
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
/// <returns>NULL 失败</returns>
mgopack_ctx *mongo_find(mongo_ctx *mongo, char *filter, size_t flens, char *options);
/// <summary>
/// aggregate 命令 聚合
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="pipeline">[ stage, ... ] 聚合管道阶段数组</param>
/// <param name="pllens">pipeline长度</param>
/// <param name="options">可选 其他参数 document 
/// (explain allowDiskUse maxTimeMS bypassDocumentValidation readConcern collation hint comment writeConcern let)
/// </param>
/// <returns>NULL 失败</returns>
mgopack_ctx *mongo_aggregate(mongo_ctx *mongo, char *pipeline, size_t pllens, char *options);
/// <summary>
/// getMore 命令 返回游标当前指向的文档的后续批次
/// </summary>
/// <param name="mgpack">mgopack_ctx</param>
/// <param name="cursorid">游标标识符</param>
/// <param name="options">可选 其他参数 document (collection batchSize maxTimeMS comment)</param>
/// <returns>NULL 失败</returns>
mgopack_ctx *mongo_getmore(mongo_ctx *mongo, int64_t cursorid, char *options);
/// <summary>
/// killCursors 命令 终止集合的一个或多个指定游标  MORETOCOME 可用
/// </summary>
/// <param name="mgpack">mgopack_ctx</param>
/// <param name="cursorids">游标标识符 [cursorid, ...]</param>
/// <param name="cslens">cursorids长度</param>
/// <param name="options">可选 其他参数 document (comment)</param>
/// <returns>设置MORETOCOME始终返回NULL, 未设置则 NULL 失败</returns>
mgopack_ctx *mongo_killcursors(mongo_ctx *mongo, char *cursorids, size_t cslens, char *options);
/// <summary>
/// distinct 命令 查找单个集合中指定字段的不同值
/// </summary>
/// <param name="mgpack">mgopack_ctx</param>
/// <param name="key">字段</param>
/// <param name="query">可选 查询 document </param>
/// <param name="qlens">query长度</param>
/// <param name="options">可选 其他参数 document (readConcern collation comment hint)</param>
/// <returns>NULL 失败</returns>
mgopack_ctx *mongo_distinct(mongo_ctx *mongo, const char *key, char *query, size_t qlens, char *options);
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
/// <returns>NULL 失败</returns>
mgopack_ctx *mongo_findandmodify(mongo_ctx *mongo, char *query, size_t qlens,
    int32_t remove, int32_t pipeline, char *update, size_t ulens, char *options);
/// <summary>
/// count 命令 计算集合或视图中的文档数量
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="query">可选 查询，选择哪些文档要在集合或视图中计数</param>
/// <param name="qlens,">query长度</param>
/// <param name="options">可选 其他参数 document (limit skip hint readConcern maxTimeMS collation comment)</param>
/// <returns>ERR_OK 成功</returns>
int32_t mongo_count(mongo_ctx *mongo, char *query, size_t qlens, char *options);
/// <summary>
/// createindexes 命令 为集合构建一个或多个索引  MORETOCOME 可用
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="indexes">指定要创建的索引[{key:{...},name:}...]</param>
/// <param name="ilens">indexes长度</param>
/// <param name="options">可选 其他参数 document (writeConcern commitQuorum comment)</param>
/// <returns>ERR_OK 成功</returns>
int32_t mongo_createindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options);
/// <summary>
/// dropindexes 命令 从集合中删除索引  MORETOCOME 可用
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="indexes">要删除的一个或多个索引 <arrayofstrings></param>
/// <param name="ilens">indexes长度</param>
/// <param name="options">可选 其他参数 document (writeConcern comment)</param>
/// <returns>ERR_OK 成功</returns>
int32_t mongo_dropindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options);
/// <summary>
/// startsession 命令 启动新会话
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <returns>NULL 失败 mongo_session</returns>
mongo_session *mongo_startsession(mongo_ctx *mongo);
/// <summary>
/// refreshsession 命令 刷新空闲会话
/// </summary>
/// <param name="session">mongo_session</param>
/// <returns>ERR_OK 成功</returns>
int32_t mongo_refreshsession(mongo_session *session);
/// <summary>
/// endsessions 命令 使会话过期,释放mongo_session
/// </summary>
/// <param name="session">mongo_session</param>
void mongo_freesession(mongo_session *session);
/// <summary>
/// 事务开始
/// </summary>
/// <param name="session">mongo_session</param>
void mongo_begin(mongo_session *session);
/// <summary>
/// 事务提交
/// </summary>
/// <param name="session">mongo_session</param>
/// <param name="options">可选 其他参数 document (writeConcern comment)</param>
/// <returns>ERR_OK 成功</returns>
int32_t mongo_commit(mongo_session *session, char *options);
/// <summary>
/// 事务回滚
/// </summary>
/// <param name="session">mongo_session</param>
/// <param name="options">可选 其他参数 document (writeConcern comment)</param>
/// <returns>ERR_OK 成功</returns>
int32_t mongo_rollback(mongo_session *session, char *options);

#endif//CORO_UTILS_H_
