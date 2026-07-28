#include "protocol/mongo/mongo_pack.h"
#include "serial/bson.h"
#include "utils/utils.h"
#include "utils/binary.h"
#include "crypt/scram.h"

// commitTransaction / abortTransaction 按规范只能发往 admin 库，与连接当前的 $db 无关；
// 发错库服务端回 code 13 Unauthorized "may only be run against the admin database"
#define MONGO_TXN_DB "admin"
#define BSON_HEADROOM 256          // 大消息 cap 估算余量（命令名+集合名+元数据+session options 等）
// 拼接 options：bson_cat 失败即源文档达 MAX_PACK_SIZE 被整篇丢弃，此时整条命令作废——
// 继续打包会发出缺 options 的命令，服务端照常执行并返回错误结果集。
// *size 显式置 0：调用方按"返回非 NULL 才读 size"约定，早退路径不能留未初始化值
#define MONGO_PACK_CAT(doc) do { \
        if (ERR_OK != bson_cat(&bson, (doc))) { \
            *size = 0; \
            BSON_FREE(&bson); \
            return NULL; \
        } \
    } while (0)
//事务和操作 https://www.mongodb.com/zh-cn/docs/manual/core/transactions-operations/#crud-operations
// 只带事务上下文(lsid/txnNumber/autocommit)。hello 与 commit/abort 用：commitTransaction 与
// abortTransaction 按规范不得携带 startTransaction，hello 则根本不是事务命令
#define TRANSACTION_OPTIONS \
    if (NULL != mongo->session) {\
        MONGO_PACK_CAT(mongo->session->options);\
    }
// 事务内 CRUD 用：事务的第一条命令必须带 startTransaction:true，服务端才真正开启事务；
// 缺它则该操作以 NoSuchTransaction("active transaction number is -1")失败，整个事务无从开始。
// 必须放在本函数所有 MONGO_PACK_CAT 之后——一旦置位 started 就不能再有失败早退，否则包没发出去
// 而标志已消耗，后续操作都不带 startTransaction。此位置之后只剩 MONGO_PACK_RETURN，它不会失败。
// 残留边界：置位后 coro_send 若网络失败，事务在服务端并未开启而 started 已为 1，该 session
// 只能重新 begin；这与"连接断开后 session 失效需重建"的既有约定一致
#define TRANSACTION_OPTIONS_START \
    if (NULL != mongo->session) {\
        MONGO_PACK_CAT(mongo->session->options);\
        if (0 == mongo->session->started) {\
            bson_append_bool(&bson, "startTransaction", 1);\
            mongo->session->started = 1;\
        }\
    }
// 函数开头：声明并初始化局部 bson_ctx bson（必须置于函数体顶部）
// cap：BSON 预估容量，0=默认；大消息传 dlens + BSON_HEADROOM 消除 doubling 重分配
#define MONGO_PACK_BEGIN(cap) \
    bson_ctx bson; \
    bson_init(&bson, NULL, (cap))
// 函数收尾：写入 $db + 闭合 + 打包 OP_MSG + 释放 bson + return；db 形参为 mongo->db / mongo->authdb 等
#define MONGO_PACK_RETURN(db) do { \
        bson_append_utf8(&bson, "$db", (db)); \
        bson_append_end(&bson); \
        void *_data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size); \
        BSON_FREE(&bson); \
        return _data; \
    } while (0)

// 构造 OP_MSG 原始数据包：填充消息头、flags、Section 和正文，并回填总长度
static void *_mongo_pack_msg(mongo_ctx *mongo, int32_t kind, const char *docid, char *docs, size_t dlens, size_t *size) {
    mongo->reqid++;
    binary_ctx bwriter;
    size_t init_cap = 17 + dlens + (1 == kind ? 4 + strlen(docid) + 1 : 0);
    binary_init(&bwriter, NULL, init_cap, 0);
    binary_set_skip(&bwriter, 4);//size
    binary_set_integer(&bwriter, mongo->reqid, 4, 1);//reqid
    binary_set_integer(&bwriter, 0, 4, 1);//respto
    binary_set_integer(&bwriter, OP_MSG, 4, 1);//prot
    binary_set_integer(&bwriter, mongo->flags, 4, 1);//flags
    if (0 == kind) {
        binary_set_int8(&bwriter, 0);//kind
    } else {
        binary_set_int8(&bwriter, 1);//kind
        binary_set_integer(&bwriter, 4 + strlen(docid) + 1 + dlens, 4, 1);
        binary_set_string(&bwriter, docid);
    }
    binary_set_binary(&bwriter, docs, dlens);//正文
    *size = bwriter.offset;
    binary_offset(&bwriter, 0);
    binary_set_integer(&bwriter, *size, 4, 1);
    binary_offset(&bwriter, *size);
    return bwriter.data;
}
void *mongo_pack_scram_client_first(mongo_ctx *mongo, const char *method, size_t *size) {
    *size = 0;
    if (0 == strlen(mongo->authdb)) {
        safe_fill_str(mongo->authdb, sizeof(mongo->authdb), mongo->db);
    }
    if (0 == strlen(mongo->user)
        || 0 == strlen(mongo->password)
        || 0 == strlen(mongo->authdb)
        || NULL != mongo->scram) {
        return NULL;
    }
    mongo->scram = scram_init(method, 1);
    if (NULL == mongo->scram) {
        return NULL;
    }
    if (ERR_OK != scram_set_user(mongo->scram, mongo->user, strlen(mongo->user))) {
        scram_free(mongo->scram);
        mongo->scram = NULL;
        return NULL;
    }
    char *first_message = scram_first_message(mongo->scram);
    if (NULL == first_message) {
        scram_free(mongo->scram);
        mongo->scram = NULL;
        return NULL;
    }
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "saslStart", 1);
    bson_append_utf8(&bson, "mechanism", method);
    bson_append_binary(&bson, "payload", BSON_SUBTYPE_BINARY, first_message, strlen(first_message));
    FREE(first_message);
    bson_append_int32(&bson, "autoAuthorize", 1);
    bson_append_document_begain(&bson, "options");
    bson_append_bool(&bson, "skipEmptyExchange", 1);
    bson_append_end(&bson);//options
    MONGO_PACK_RETURN(mongo->authdb);
}
void *mongo_pack_scram_client_final(mongo_ctx *mongo, int32_t convid, char *client_final, size_t *size) {
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "saslContinue", 1);
    bson_append_int32(&bson, "conversationId", convid);
    bson_append_binary(&bson, "payload", BSON_SUBTYPE_BINARY, client_final, strlen(client_final));
    MONGO_PACK_RETURN(mongo->authdb);
}
void *mongo_pack_hello(mongo_ctx *mongo, char *options, size_t *size) {
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "hello", 1);//不能是事务中的第一项操作
    bson_append_document_begain(&bson, "comment");
    bson_append_utf8(&bson, "application", "srey");
    bson_append_utf8(&bson, "os", OS_NAME);
    bson_append_end(&bson);//comment
    TRANSACTION_OPTIONS
    MONGO_PACK_CAT(options);
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_ping(mongo_ctx *mongo, size_t *size) {
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "ping", 1);
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_drop(mongo_ctx *mongo, char *options, size_t *size) {
    MONGO_PACK_BEGIN(0);
    bson_append_utf8(&bson, "drop", mongo->collection);
    MONGO_PACK_CAT(options);
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_insert(mongo_ctx *mongo, char *docs, size_t dlens, char *options, size_t *size) {
    MONGO_PACK_BEGIN(dlens + BSON_HEADROOM);
    bson_append_utf8(&bson, "insert", mongo->collection);
    bson_append_array(&bson, "documents", docs, dlens);
    MONGO_PACK_CAT(options);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_update(mongo_ctx *mongo, char *updates, size_t ulens, char *options, size_t *size) {
    MONGO_PACK_BEGIN(ulens + BSON_HEADROOM);
    bson_append_utf8(&bson, "update", mongo->collection);
    bson_append_array(&bson, "updates", updates, ulens);
    MONGO_PACK_CAT(options);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_delete(mongo_ctx *mongo, char *deletes, size_t dlens, char *options, size_t *size) {
    MONGO_PACK_BEGIN(dlens + BSON_HEADROOM);
    bson_append_utf8(&bson, "delete", mongo->collection);
    bson_append_array(&bson, "deletes", deletes, dlens);
    MONGO_PACK_CAT(options);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_bulkwrite(mongo_ctx *mongo, char *ops, size_t olens, char *nsinfo, size_t nlens, char *options, size_t *size) {
    MONGO_PACK_BEGIN(olens + nlens + BSON_HEADROOM);
    bson_append_int32(&bson, "bulkWrite", 1);
    bson_append_array(&bson, "ops", ops, olens);
    bson_append_array(&bson, "nsInfo", nsinfo, nlens);
    MONGO_PACK_CAT(options);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_find(mongo_ctx *mongo, char *filter, size_t flens, char *options, size_t *size) {
    MONGO_PACK_BEGIN(flens + BSON_HEADROOM);
    bson_append_utf8(&bson, "find", mongo->collection);
    if (NULL != filter) {
        bson_append_document(&bson, "filter", filter, flens);
    }
    MONGO_PACK_CAT(options);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_aggregate(mongo_ctx *mongo, char *pipeline, size_t pllens, char *options, size_t *size) {
    MONGO_PACK_BEGIN(pllens + BSON_HEADROOM);
    bson_append_utf8(&bson, "aggregate", mongo->collection);
    bson_append_array(&bson, "pipeline", pipeline, pllens);
    const char *cursor = bson_empty(size);
    bson_append_document(&bson, "cursor", (char *)cursor, *size);
    MONGO_PACK_CAT(options);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_getmore(mongo_ctx *mongo, int64_t cursorid, char *options, size_t *size) {
    MONGO_PACK_BEGIN(0);
    bson_append_int64(&bson, "getMore", cursorid);//事务外部创建的游标，无法在事务内部调用 getMore
    bson_append_utf8(&bson, "collection", mongo->collection);
    MONGO_PACK_CAT(options);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_killcursors(mongo_ctx *mongo, char *cursorids, size_t cslens, char *options, size_t *size) {
    MONGO_PACK_BEGIN(cslens + BSON_HEADROOM);
    bson_append_utf8(&bson, "killCursors", mongo->collection);//不能将killCursors 命令指定为ACID 事务中的第一个操作.killCursors 命令，服务器会立即停止指定的游标。它不会等待ACID 事务提交
    bson_append_array(&bson, "cursors", cursorids, cslens);
    MONGO_PACK_CAT(options);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_distinct(mongo_ctx *mongo, const char *key, char *query, size_t qlens, char *options, size_t *size) {
    MONGO_PACK_BEGIN(qlens + BSON_HEADROOM);
    bson_append_utf8(&bson, "distinct", mongo->collection);
    bson_append_utf8(&bson, "key", key);
    if (NULL != query) {
        bson_append_document(&bson, "query", query, qlens);
    }
    MONGO_PACK_CAT(options);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_findandmodify(mongo_ctx *mongo, char *query, size_t qlens, int32_t remove, int32_t pipeline, char *update, size_t ulens,
    char *options, size_t *size) {
    MONGO_PACK_BEGIN(qlens + ulens + BSON_HEADROOM);
    bson_append_utf8(&bson, "findAndModify", mongo->collection);
    if (NULL != query) {
        bson_append_document(&bson, "query", query, qlens);
    }
    if (remove) {
        bson_append_bool(&bson, "remove", 1);//默认值为 false
    } else {
        if (pipeline) {
            bson_append_array(&bson, "update", update, ulens);
        } else {
            bson_append_document(&bson, "update", update, ulens);
        }
    }
    MONGO_PACK_CAT(options);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_count(mongo_ctx *mongo, char *query, size_t qlens, char *options, size_t *size) {
    MONGO_PACK_BEGIN(qlens + BSON_HEADROOM);
    bson_append_utf8(&bson, "count", mongo->collection);
    if (NULL != query) {
        bson_append_document(&bson, "query", query, qlens);
    }
    MONGO_PACK_CAT(options);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_createindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t *size) {
    MONGO_PACK_BEGIN(ilens + BSON_HEADROOM);
    bson_append_utf8(&bson, "createIndexes", mongo->collection);
    bson_append_array(&bson, "indexes", indexes, ilens);
    MONGO_PACK_CAT(options);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_dropindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t *size) {
    MONGO_PACK_BEGIN(ilens + BSON_HEADROOM);
    bson_append_utf8(&bson, "dropIndexes", mongo->collection);
    bson_append_array(&bson, "index", indexes, ilens);
    MONGO_PACK_CAT(options);
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_startsession(mongo_ctx *mongo, size_t *size) {
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "startSession", 1);
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_refreshsession(mongo_session *session, size_t *size) {
    mongo_ctx *mongo = session->mongo;
    MONGO_PACK_BEGIN(0);
    bson_append_array_begain(&bson, "refreshSessions");
    bson_append_document_begain(&bson, "0");
    bson_append_binary(&bson, "id", BSON_SUBTYPE_UUID, session->uuid, UUID_LENS);
    bson_append_end(&bson);//0
    bson_append_end(&bson);//refreshSessions
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_endsession(mongo_session *session, size_t *size) {
    mongo_ctx *mongo = session->mongo;
    MONGO_PACK_BEGIN(0);
    bson_append_array_begain(&bson, "endSessions");
    bson_append_document_begain(&bson, "0");
    bson_append_binary(&bson, "id", BSON_SUBTYPE_UUID, session->uuid, UUID_LENS);
    bson_append_end(&bson);//0
    bson_append_end(&bson);//endSessions
    MONGO_PACK_RETURN(mongo->db);
}
char *mongo_transaction_options(mongo_session *session) {
    MONGO_PACK_BEGIN(0);
    bson_append_document_begain(&bson, "lsid");
    bson_append_binary(&bson, "id", BSON_SUBTYPE_UUID, session->uuid, UUID_LENS);
    bson_append_end(&bson);//lsid
    bson_append_int64(&bson, "txnNumber", session->txnnumber);
    bson_append_bool(&bson, "autocommit", 0);
    bson_append_end(&bson);
    return bson.doc.data;
}
void *mongo_pack_committransaction(mongo_session *session, char *options, size_t *size) {
    mongo_ctx *mongo = session->mongo;
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "commitTransaction", 1);
    TRANSACTION_OPTIONS
    MONGO_PACK_CAT(options);
    MONGO_PACK_RETURN(MONGO_TXN_DB);
}
void *mongo_pack_aborttransaction(mongo_session *session, char *options, size_t *size) {
    mongo_ctx *mongo = session->mongo;
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "abortTransaction", 1);
    TRANSACTION_OPTIONS
    MONGO_PACK_CAT(options);
    MONGO_PACK_RETURN(MONGO_TXN_DB);
}
