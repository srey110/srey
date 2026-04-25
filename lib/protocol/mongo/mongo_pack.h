#ifndef MONGO_PACK_H_
#define MONGO_PACK_H_

#include "protocol/mongo/mongo_struct.h"
//https://www.mongodb.com/zh-cn/docs/manual/reference/command/

/// <summary>
/// 构造 SCRAM 认证第一步（saslStart）请求包
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="method">认证机制，如 "SCRAM-SHA-1" 或 "SCRAM-SHA-256"</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放；NULL 表示条件不满足或初始化失败</returns>
void *mongo_pack_scram_client_first(mongo_ctx *mongo, const char *method, size_t *size);
/// <summary>
/// 构造 SCRAM 认证第二步（saslContinue）请求包
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="convid">服务端返回的 conversationId</param>
/// <param name="client_final">客户端最终消息字符串</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_scram_client_final(mongo_ctx *mongo, int32_t convid, char *client_final, size_t *size);

/// <summary>
/// 构造 hello 握手命令请求包（用于建立连接后的能力协商）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="options">附加的 BSON 选项数据，NULL 表示无</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_hello(mongo_ctx *mongo, char *options, size_t *size);
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
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_drop(mongo_ctx *mongo, char *options, size_t *size);
/// <summary>
/// 构造 insert 命令请求包（插入文档）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="docs">BSON 数组格式的待插入文档列表</param>
/// <param name="dlens">docs 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_insert(mongo_ctx *mongo, char *docs, size_t dlens, char *options, size_t *size);
/// <summary>
/// 构造 update 命令请求包（更新文档）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="updates">BSON 数组格式的更新操作列表</param>
/// <param name="ulens">updates 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_update(mongo_ctx *mongo, char *updates, size_t ulens, char *options, size_t *size);
/// <summary>
/// 构造 delete 命令请求包（删除文档）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="deletes">BSON 数组格式的删除操作列表</param>
/// <param name="dlens">deletes 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_delete(mongo_ctx *mongo, char *deletes, size_t dlens, char *options, size_t *size);
/// <summary>
/// 构造 bulkWrite 命令请求包（批量写操作，MongoDB 8.0+）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="ops">BSON 数组格式的操作列表</param>
/// <param name="olens">ops 数据长度</param>
/// <param name="nsinfo">BSON 数组格式的命名空间信息列表</param>
/// <param name="nlens">nsinfo 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_bulkwrite(mongo_ctx *mongo, char *ops, size_t olens, char *nsinfo, size_t nlens, char *options, size_t *size);
/// <summary>
/// 构造 find 命令请求包（查询文档）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="filter">BSON 文档格式的过滤条件，NULL 表示无过滤</param>
/// <param name="flens">filter 数据长度</param>
/// <param name="options">附加 BSON 选项（如 sort/projection/limit），NULL 表示无</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_find(mongo_ctx *mongo, char *filter, size_t flens, char *options, size_t *size);
/// <summary>
/// 构造 aggregate 命令请求包（聚合查询）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="pipeline">BSON 数组格式的聚合管道阶段列表</param>
/// <param name="pllens">pipeline 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_aggregate(mongo_ctx *mongo, char *pipeline, size_t pllens, char *options, size_t *size);
/// <summary>
/// 构造 getMore 命令请求包（获取游标后续批次）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="cursorid">游标 ID</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_getmore(mongo_ctx *mongo, int64_t cursorid, char *options, size_t *size);
/// <summary>
/// 构造 killCursors 命令请求包（关闭游标）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="cursorids">BSON 数组格式的游标 ID 列表</param>
/// <param name="cslens">cursorids 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_killcursors(mongo_ctx *mongo, char *cursorids, size_t cslens, char *options, size_t *size);
/// <summary>
/// 构造 distinct 命令请求包（获取字段唯一值列表）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="key">去重字段名</param>
/// <param name="query">BSON 文档格式的过滤条件，NULL 表示无过滤</param>
/// <param name="qlens">query 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_distinct(mongo_ctx *mongo, const char *key, char *query, size_t qlens, char *options, size_t *size);
/// <summary>
/// 构造 findAndModify 命令请求包（原子查找并修改/删除）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="query">BSON 文档格式的查询条件，NULL 表示无</param>
/// <param name="qlens">query 数据长度</param>
/// <param name="remove">非零表示删除匹配文档（与 update 互斥）</param>
/// <param name="pipeline">非零表示 update 为聚合管道数组格式</param>
/// <param name="update">BSON 格式的更新操作或聚合管道（remove 为 0 时有效）</param>
/// <param name="ulens">update 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_findandmodify(mongo_ctx *mongo, char *query, size_t qlens, int32_t remove, int32_t pipeline, char *update, size_t ulens,
    char *options, size_t *size);
/// <summary>
/// 构造 count 命令请求包（统计文档数量）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="query">BSON 文档格式的过滤条件，NULL 表示全部</param>
/// <param name="qlens">query 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_count(mongo_ctx *mongo, char *query, size_t qlens, char *options, size_t *size);

/// <summary>
/// 构造 createIndexes 命令请求包（创建索引）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="indexes">BSON 数组格式的索引定义列表</param>
/// <param name="ilens">indexes 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_createindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t *size);
/// <summary>
/// 构造 dropIndexes 命令请求包（删除索引）
/// </summary>
/// <param name="mongo">mongo_ctx</param>
/// <param name="indexes">BSON 数组格式的索引名列表</param>
/// <param name="ilens">indexes 数据长度</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_dropindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t *size);

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
/// 构造 endSessions 命令请求包（结束会话）
/// </summary>
/// <param name="session">mongo_session</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_endsession(mongo_session *session, size_t *size);
/// <summary>
/// 构建事务选项 BSON 数据（含 lsid/txnNumber/autocommit），用于事务中的每条命令
/// </summary>
/// <param name="session">mongo_session</param>
/// <returns>BSON 数据指针，需调用者释放</returns>
char *mongo_transaction_options(mongo_session *session);
/// <summary>
/// 构造 commitTransaction 命令请求包（提交事务）
/// </summary>
/// <param name="session">mongo_session</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_committransaction(mongo_session *session, char *options, size_t *size);
/// <summary>
/// 构造 abortTransaction 命令请求包（回滚事务）
/// </summary>
/// <param name="session">mongo_session</param>
/// <param name="options">附加 BSON 选项，NULL 表示无</param>
/// <param name="size">输出数据包长度</param>
/// <returns>数据包指针，需调用者释放</returns>
void *mongo_pack_aborttransaction(mongo_session *session, char *options, size_t *size);

#endif//MONGO_PACK_H_
