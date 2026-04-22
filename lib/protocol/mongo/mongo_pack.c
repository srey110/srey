#include "protocol/mongo/mongo_pack.h"
#include "protocol/mongo/bson.h"
#include "utils/utils.h"
#include "utils/binary.h"
#include "crypt/scram.h"

//事务和操作 https://www.mongodb.com/zh-cn/docs/manual/core/transactions-operations/#crud-operations
#define TRANSACTION_OPTIONS \
    if (NULL != mongo->session) {\
        bson_cat(&bson, mongo->session->options);\
    }

static void *_mongo_pack_msg(mongo_ctx *mongo, int32_t kind, const char *docid, char *docs, size_t dlens, size_t *size) {
    mongo->reqid++;
    binary_ctx bwriter;
    binary_init(&bwriter, 0, 0, 0);
    binary_set_skip(&bwriter, 4);//size
    binary_set_integer(&bwriter, mongo->reqid, 4, 1);//reqid
    binary_set_integer(&bwriter, 0, 4, 1);//respto
    binary_set_integer(&bwriter, OP_MSG, 4, 1);//prot
    binary_set_integer(&bwriter, mongo->flags, 4, 1);//flags
    if (0 == kind) {
        binary_set_int8(&bwriter, 0);//kind
    }
    else {
        binary_set_int8(&bwriter, 1);//kind
        binary_set_integer(&bwriter, 4 + strlen(docid) + 1 + dlens, 4, 1);
        binary_set_string(&bwriter, docid, 0);
    }
    binary_set_string(&bwriter, docs, dlens);//正文
    *size = bwriter.offset;
    binary_offset(&bwriter, 0);
    binary_set_integer(&bwriter, *size, 4, 1);
    binary_offset(&bwriter, *size);
    return bwriter.data;
}
void *mongo_pack_scram_client_first(mongo_ctx *mongo, const char *method, size_t *size) {
    if (0 == strlen(mongo->authdb)) {
        strcpy(mongo->authdb, mongo->db);
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
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_int32(&bson, "saslStart", 1);
    bson_append_utf8(&bson, "mechanism", method);
    scram_set_user(mongo->scram, mongo->user);
    char *first_message = scram_first_message(mongo->scram);
    bson_append_binary(&bson, "payload", BSON_SUBTYPE_BINARY, first_message, strlen(first_message));
    FREE(first_message);
    bson_append_int32(&bson, "autoAuthorize", 1);
    bson_append_document_begain(&bson, "options");
    bson_append_bool(&bson, "skipEmptyExchange", 1);
    bson_append_end(&bson);//options
    bson_append_utf8(&bson, "$db", mongo->authdb);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_scram_client_final(mongo_ctx *mongo, int32_t convid, char *client_final, size_t *size) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_int32(&bson, "saslContinue", 1);
    bson_append_int32(&bson, "conversationId", convid);
    bson_append_binary(&bson, "payload", BSON_SUBTYPE_BINARY, client_final, strlen(client_final));
    bson_append_utf8(&bson, "$db", mongo->authdb);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_hello(mongo_ctx *mongo, char *options, size_t *size) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_int32(&bson, "hello", 1);//不能是事务中的第一项操作
    bson_append_document_begain(&bson, "comment");
    bson_append_utf8(&bson, "application", "srey");
    bson_append_utf8(&bson, "os", OS_NAME);
    bson_append_end(&bson);//comment
    TRANSACTION_OPTIONS
        bson_cat(&bson, options);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_ping(mongo_ctx *mongo, size_t *size) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_int32(&bson, "ping", 1);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_drop(mongo_ctx *mongo, char *options, size_t *size) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_utf8(&bson, "drop", mongo->collection);
    bson_cat(&bson, options);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_insert(mongo_ctx *mongo, char *docs, size_t dlens, char *options, size_t *size) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_utf8(&bson, "insert", mongo->collection);
    bson_append_array(&bson, "documents", docs, dlens);
    TRANSACTION_OPTIONS
        bson_cat(&bson, options);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_update(mongo_ctx *mongo, char *updates, size_t ulens, char *options, size_t *size) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_utf8(&bson, "update", mongo->collection);
    bson_append_array(&bson, "updates", updates, ulens);
    TRANSACTION_OPTIONS
        bson_cat(&bson, options);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_delete(mongo_ctx *mongo, char *deletes, size_t dlens, char *options, size_t *size) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_utf8(&bson, "delete", mongo->collection);
    bson_append_array(&bson, "deletes", deletes, dlens);
    TRANSACTION_OPTIONS
        bson_cat(&bson, options);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_bulkwrite(mongo_ctx *mongo, char *ops, size_t olens, char *nsinfo, size_t nlens, char *options, size_t *size) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_int32(&bson, "bulkWrite", 1);
    bson_append_array(&bson, "ops", ops, olens);
    bson_append_array(&bson, "nsInfo", nsinfo, nlens);
    TRANSACTION_OPTIONS
        bson_cat(&bson, options);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_find(mongo_ctx *mongo, char *filter, size_t flens, char *options, size_t *size) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_utf8(&bson, "find", mongo->collection);
    if (NULL != filter) {
        bson_append_document(&bson, "filter", filter, flens);
    }
    TRANSACTION_OPTIONS
        bson_cat(&bson, options);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_aggregate(mongo_ctx *mongo, char *pipeline, size_t pllens, char *options, size_t *size) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_utf8(&bson, "aggregate", mongo->collection);
    bson_append_array(&bson, "pipeline", pipeline, pllens);
    const char *cursor = bson_empty(size);
    bson_append_document(&bson, "cursor", (char *)cursor, *size);
    TRANSACTION_OPTIONS
        bson_cat(&bson, options);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_getmore(mongo_ctx *mongo, int64_t cursorid, char *options, size_t *size) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_int64(&bson, "getMore", cursorid);//事务外部创建的游标，无法在事务内部调用 getMore
    bson_append_utf8(&bson, "collection", mongo->collection);
    TRANSACTION_OPTIONS
        bson_cat(&bson, options);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_killcursors(mongo_ctx *mongo, char *cursorids, size_t cslens, char *options, size_t *size) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_utf8(&bson, "killCursors", mongo->collection);//不能将killCursors 命令指定为ACID 事务中的第一个操作.killCursors 命令，服务器会立即停止指定的游标。它不会等待ACID 事务提交
    bson_append_array(&bson, "cursors", cursorids, cslens);
    TRANSACTION_OPTIONS
        bson_cat(&bson, options);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_distinct(mongo_ctx *mongo, const char *key, char *query, size_t qlens, char *options, size_t *size) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_utf8(&bson, "distinct", mongo->collection);
    bson_append_utf8(&bson, "key", key);
    if (NULL != query) {
        bson_append_document(&bson, "query", query, qlens);
    }
    TRANSACTION_OPTIONS
        bson_cat(&bson, options);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_findandmodify(mongo_ctx *mongo, char *query, size_t qlens, int32_t remove, int32_t pipeline, char *update, size_t ulens,
    char *options, size_t *size) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_utf8(&bson, "findAndModify", mongo->collection);
    if (NULL != query) {
        bson_append_document(&bson, "query", query, qlens);
    }
    if (remove) {
        bson_append_bool(&bson, "remove", 1);//默认值为 false
    }
    else {
        if (pipeline) {
            bson_append_array(&bson, "update", update, ulens);
        }
        else {
            bson_append_document(&bson, "update", update, ulens);
        }
    }
    TRANSACTION_OPTIONS
        bson_cat(&bson, options);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_count(mongo_ctx *mongo, char *query, size_t qlens, char *options, size_t *size) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_utf8(&bson, "count", mongo->collection);
    if (NULL != query) {
        bson_append_document(&bson, "query", query, qlens);
    }
    TRANSACTION_OPTIONS
        bson_cat(&bson, options);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_createindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t *size) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_utf8(&bson, "createIndexes", mongo->collection);
    bson_append_array(&bson, "indexes", indexes, ilens);
    TRANSACTION_OPTIONS
        bson_cat(&bson, options);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_dropindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t *size) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_utf8(&bson, "dropIndexes", mongo->collection);
    bson_append_array(&bson, "index", indexes, ilens);
    bson_cat(&bson, options);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_startsession(mongo_ctx *mongo, size_t *size) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_int32(&bson, "startSession", 1);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_refreshsession(mongo_session *session, size_t *size) {
    mongo_ctx *mongo = session->mongo;
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_array_begain(&bson, "refreshSessions");
    bson_append_document_begain(&bson, "0");
    bson_append_binary(&bson, "id", BSON_SUBTYPE_UUID, session->uuid, UUID_LENS);
    bson_append_end(&bson);//0
    bson_append_end(&bson);//refreshSessions
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_endsession(mongo_session *session, size_t *size) {
    mongo_ctx *mongo = session->mongo;
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_array_begain(&bson, "endSessions");
    bson_append_document_begain(&bson, "0");
    bson_append_binary(&bson, "id", BSON_SUBTYPE_UUID, session->uuid, UUID_LENS);
    bson_append_end(&bson);//0
    bson_append_end(&bson);//endSessions
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
char *mongo_transaction_options(mongo_session *session) {
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
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
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_int32(&bson, "commitTransaction", 1);
    TRANSACTION_OPTIONS
        bson_cat(&bson, options);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
void *mongo_pack_aborttransaction(mongo_session *session, char *options, size_t *size) {
    mongo_ctx *mongo = session->mongo;
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_int32(&bson, "abortTransaction", 1);
    TRANSACTION_OPTIONS
        bson_cat(&bson, options);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = _mongo_pack_msg(mongo, 0, NULL, bson.doc.data, bson.doc.offset, size);
    BSON_FREE(&bson);
    return data;
}
