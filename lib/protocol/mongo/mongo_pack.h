#ifndef MONGO_PACK_H_
#define MONGO_PACK_H_

#include "protocol/mongo/mongo_struct.h"

#define MONGO_TXN_NONE UINT64_MAX // mongo_txn_mark 的"没绑事务"；skid 来自 createid，最高位恒 0，撞不上
//https://www.mongodb.com/zh-cn/docs/manual/reference/command/
// 包头 flags：drop insert update delete bulkWrite killCursors createIndexes dropIndexes 照抄连接级
// flags（MORETOCOME 可用）；endSessions 恒带 MORETOCOME；其余命令都要等回包，恒不带。
// 连接上绑着的事务已随重连作废（连接换过）时，带事务上下文的命令一律组包失败返回 NULL；
// 例外是 abortTransaction（照发，让服务端当场中止放锁）与提交已交出过的 commitTransaction（可重发，
// 见 mongo_session_stale）。hello 与 count 从不带事务上下文。
// 组包在锁外、发送在拿到锁之后，中间可能重连：带事务上下文的包发送前须经 mongo_txn_moved 复查

/// <summary>
/// 读回已组好的数据包里写着的消息标志位。
/// "要不要等回包"必须问这个而不是 mongo_check_flag：后者读的是连接级可变字段，
/// 组包与真正发送之间隔着一次会挂起的加锁，期间 set/clear 一改，包里写的和判定读的就是两回事
/// </summary>
/// <param name="pack">mongo_pack_* 组出的数据包；NULL 返回 0</param>
/// <param name="flag">mongo_flags 标志位</param>
/// <returns>非零表示该包写着此标志位</returns>
int32_t mongo_pack_check_flag(void *pack, mongo_flags flag);
/// <summary>
/// 构造 SCRAM 认证第一步（saslStart）请求包，同时新建本次认证用的 scram 上下文。
/// 不碰 mongo->scram：scram 须经 mongo_auth_begin 交给连接所在的网络线程装上，再发本包。
/// authdb 为空时先用 db 填上
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="method">认证机制，如 "SCRAM-SHA-1" 或 "SCRAM-SHA-256"</param>
/// <param name="scram">输出：新建的 scram，所有权归调用方（通常直接交给 mongo_auth_begin）；返回 NULL 时不写。必须非 NULL</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放；用户名 / 密码 / 认证库缺一、算法不支持或初始化失败返回 NULL，此时没有新建的 scram</returns>
void *mongo_pack_scram_client_first(mongo_ctx *mongo, const char *method, struct scram_ctx **scram, size_t *size);
/// <summary>
/// 构造 SCRAM 认证第二步（saslContinue）请求包
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="convid">服务端返回的 conversationId</param>
/// <param name="client_final">客户端最终消息，按 flens 定界，不要求 NUL 结尾</param>
/// <param name="flens">client_final 字节数</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_scram_client_final(mongo_ctx *mongo, int32_t convid, char *client_final, size_t flens, size_t *size);
/// <summary>
/// 构造 hello 握手命令请求包（用于建立连接后的能力协商）。恒不带事务上下文：重连握手也用它，
/// 那时别的协程可能已在新连接上 begin
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="options">附加的 BSON 选项数据，NULL 表示无</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_hello(mongo_ctx *mongo, char *options, size_t optlens, size_t *size);
/// <summary>
/// 构造 ping 命令请求包（用于心跳检测）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_ping(mongo_ctx *mongo, size_t *size);
/// <summary>
/// 构造 drop 命令请求包（删除当前集合）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_drop(mongo_ctx *mongo, char *options, size_t optlens, size_t *size);
/// <summary>
/// 构造 insert 命令请求包（插入文档）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="docs">BSON 数组格式的待插入文档列表；必填，不是一篇完整 BSON（判据见 bson_check_doc，NULL 与长度 0 都算）时整条命令作废</param>
/// <param name="dlens">docs 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_insert(mongo_ctx *mongo, char *docs, size_t dlens, char *options, size_t optlens, size_t *size);
/// <summary>
/// 构造 update 命令请求包（更新文档）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="updates">BSON 数组格式的更新操作列表；必填，不是一篇完整 BSON（判据见 bson_check_doc，NULL 与长度 0 都算）时整条命令作废</param>
/// <param name="ulens">updates 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_update(mongo_ctx *mongo, char *updates, size_t ulens, char *options, size_t optlens, size_t *size);
/// <summary>
/// 构造 delete 命令请求包（删除文档）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="deletes">BSON 数组格式的删除操作列表；必填，不是一篇完整 BSON（判据见 bson_check_doc，NULL 与长度 0 都算）时整条命令作废</param>
/// <param name="dlens">deletes 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_delete(mongo_ctx *mongo, char *deletes, size_t dlens, char *options, size_t optlens, size_t *size);
/// <summary>
/// 构造 bulkWrite 命令请求包（批量写操作，MongoDB 8.0+）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="ops">BSON 数组格式的操作列表；必填，不是一篇完整 BSON（判据见 bson_check_doc，NULL 与长度 0 都算）时整条命令作废</param>
/// <param name="olens">ops 数据长度</param>
/// <param name="nsinfo">BSON 数组格式的命名空间信息列表；必填，不是一篇完整 BSON（判据见 bson_check_doc，NULL 与长度 0 都算）时整条命令作废</param>
/// <param name="nlens">nsinfo 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_bulkwrite(mongo_ctx *mongo, char *ops, size_t olens, char *nsinfo, size_t nlens, char *options, size_t optlens, size_t *size);
/// <summary>
/// 构造 find 命令请求包（查询文档）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="filter">BSON 文档格式的过滤条件，NULL 或长度 0 表示无过滤；给了就须是一篇完整 BSON（判据见 bson_check_doc），否则整条命令作废</param>
/// <param name="flens">filter 数据长度</param>
/// <param name="options">附加 BSON 选项（如 sort/projection/limit），NULL 表示无</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_find(mongo_ctx *mongo, char *filter, size_t flens, char *options, size_t optlens, size_t *size);
/// <summary>
/// 构造 aggregate 命令请求包（聚合查询）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="pipeline">BSON 数组格式的聚合管道阶段列表；必填，不是一篇完整 BSON（判据见 bson_check_doc，NULL 与长度 0 都算）时整条命令作废</param>
/// <param name="pllens">pipeline 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_aggregate(mongo_ctx *mongo, char *pipeline, size_t pllens, char *options, size_t optlens, size_t *size);
/// <summary>
/// 构造 getMore 命令请求包（获取游标后续批次）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="cursorid">游标 ID</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_getmore(mongo_ctx *mongo, int64_t cursorid, char *options, size_t optlens, size_t *size);
/// <summary>
/// 构造 killCursors 命令请求包（关闭游标）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="cursorids">BSON 数组格式的游标 ID 列表；必填，不是一篇完整 BSON（判据见 bson_check_doc，NULL 与长度 0 都算）时整条命令作废</param>
/// <param name="cslens">cursorids 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_killcursors(mongo_ctx *mongo, char *cursorids, size_t cslens, char *options, size_t optlens, size_t *size);
/// <summary>
/// 构造 distinct 命令请求包（获取字段唯一值列表）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="key">去重字段名</param>
/// <param name="query">BSON 文档格式的过滤条件，NULL 或长度 0 表示无过滤，口径同 mongo_pack_find 的 filter</param>
/// <param name="qlens">query 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_distinct(mongo_ctx *mongo, const char *key, char *query, size_t qlens, char *options, size_t optlens, size_t *size);
/// <summary>
/// 构造 findAndModify 命令请求包（原子查找并修改/删除）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="query">BSON 文档格式的查询条件，NULL 或长度 0 表示无，口径同 mongo_pack_find 的 filter</param>
/// <param name="qlens">query 数据长度</param>
/// <param name="remove">非零表示删除匹配文档（与 update 互斥）</param>
/// <param name="pipeline">非零表示 update 为聚合管道数组格式</param>
/// <param name="update">BSON 格式的更新操作或聚合管道（remove 为 0 时必填）；NULL 或长度 0 时整个 update 项不落笔，服务端会因缺 remove/update 明确报错；
/// remove 为 0 且给了时须是一篇完整 BSON，口径同 mongo_pack_find 的 filter（remove 非 0 时不看）</param>
/// <param name="ulens">update 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_findandmodify(mongo_ctx *mongo, char *query, size_t qlens, int32_t remove, int32_t pipeline, char *update, size_t ulens,
    char *options, size_t optlens, size_t *size);
/// <summary>
/// 构造 count 命令请求包（统计文档数量）。不带事务上下文：服务端不许 count 进事务，
/// 连接绑着事务时也按事务外执行、看不到事务里未提交的写；事务内计数用 aggregate 的 $count 阶段
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="query">BSON 文档格式的过滤条件，NULL 或长度 0 表示全部，口径同 mongo_pack_find 的 filter</param>
/// <param name="qlens">query 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_count(mongo_ctx *mongo, char *query, size_t qlens, char *options, size_t optlens, size_t *size);
/// <summary>
/// 构造 createIndexes 命令请求包（创建索引）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="indexes">BSON 数组格式的索引定义列表；必填，不是一篇完整 BSON（判据见 bson_check_doc，NULL 与长度 0 都算）时整条命令作废</param>
/// <param name="ilens">indexes 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_createindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t optlens, size_t *size);
/// <summary>
/// 构造 dropIndexes 命令请求包（删除索引）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="indexes">BSON 数组格式的索引名列表；必填，不是一篇完整 BSON（判据见 bson_check_doc，NULL 与长度 0 都算）时整条命令作废</param>
/// <param name="ilens">indexes 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_dropindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t optlens, size_t *size);
/// <summary>
/// 构造 startSession 命令请求包（开启服务端会话）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_startsession(mongo_ctx *mongo, size_t *size);
/// <summary>
/// 构造 refreshSessions 命令请求包（刷新会话以防超时）
/// </summary>
/// <param name="session">mongo_session</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_refreshsession(mongo_session *session, size_t *size);
/// <summary>
/// 构造 endSessions 命令请求包（结束会话）。包头 flags 恒为 MORETOCOME，只发不等回包；
/// 与连接级 flags 无关，也不改它
/// </summary>
/// <param name="session">mongo_session</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_endsession(mongo_session *session, size_t *size);
/// <summary>
/// 构建事务选项 BSON 数据（含 lsid/txnNumber/autocommit），用于事务中的每条命令
/// </summary>
/// <param name="session">mongo_session</param>
/// <param name="lens">输出:BSON 数据字节数</param>
/// <returns>BSON 数据指针，需调用者释放</returns>
char *mongo_transaction_options(mongo_session *session, size_t *lens);
/// <summary>
/// 本次事务是否已随重连作废：mongo_begin 之后连接换过(skid 变了)。断开前最后那条写生效没有无从确认，
/// 作废的事务不能再写、不能提交，只能中止(abortTransaction)。例外是 commitTransaction 已交给连着的连接、
/// 且本事务没发过只发不等(MORETOCOME)的写：提交结果未知但事务内容确定，不算作废，可在新连接上按同一
/// lsid/txnNumber 重发提交，由服务端裁决（已提交的照样回成功）；这时 CRUD 照旧拒绝
/// </summary>
/// <param name="session">mongo_session，须已 mongo_begin</param>
/// <returns>非 0 表示已作废</returns>
int32_t mongo_session_stale(mongo_session *session);
/// <summary>
/// 组包前记下事务现场，紧挨着带事务上下文的 mongo_pack_* 调（中间不得挂起）。
/// 不带事务上下文的命令不用记，发送前传 MONGO_TXN_NONE
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <returns>连接绑着事务时为此刻的 skid，否则 MONGO_TXN_NONE；只用来交给 mongo_txn_moved</returns>
uint64_t mongo_txn_mark(mongo_ctx *mongo);
/// <summary>
/// 拿到串行锁、发送之前复查：组包之后连接换过，包里带的是已作废事务的上下文，
/// 发到新连接上会被收进那个事务，调用方须按组包失败处理（释放包、返回失败、不发）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="mark">组包前 mongo_txn_mark 的返回值</param>
/// <returns>非 0 表示连接已换过；mark 为 MONGO_TXN_NONE 恒为 0</returns>
int32_t mongo_txn_moved(mongo_ctx *mongo, uint64_t mark);
/// <summary>
/// 构造 commitTransaction 命令请求包（提交事务）
/// </summary>
/// <param name="session">mongo_session</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放；组包成功、连接还连着且本事务没发过只发不等的写时记下"提交已交出"(见 mongo_session_stale)。
/// session 已不是连接当前绑定的那个、或事务已随重连作废时返回 NULL
/// 并置 *size 为 0，绑定不动(作废的事务该改发 abortTransaction 了结)</returns>
void *mongo_pack_committransaction(mongo_session *session, char *options, size_t optlens, size_t *size);
/// <summary>
/// 构造 abortTransaction 命令请求包（回滚事务）
/// </summary>
/// <param name="session">mongo_session</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="optlens">options 缓冲的实际字节数;options 为 NULL 时忽略</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放；session 已不是连接当前绑定的那个时返回 NULL 并置 *size 为 0。
/// 事务已随重连作废照常组包：事务挂在会话上而不是连接上，新连接上中止一样作数</returns>
void *mongo_pack_aborttransaction(mongo_session *session, char *options, size_t optlens, size_t *size);

#endif//MONGO_PACK_H_
