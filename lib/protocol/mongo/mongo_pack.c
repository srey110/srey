#include "protocol/mongo/mongo_pack.h"
#include "protocol/mongo/bson.h"
#include "utils/utils.h"
#include "utils/binary.h"
#include "crypt/scram.h"

void *mongo_pack_msg(mongo_ctx *mongo, int32_t flag, int32_t kind, const char *docid,
    const uint8_t *docs, size_t dlens, int32_t *id, size_t *size) {
    mongo->id++;
    if (NULL != id) {
        *id = mongo->id;
    }
    binary_ctx bwriter;
    binary_init(&bwriter, 0, 0, 0);
    binary_set_skip(&bwriter, 4);//size
    binary_set_integer(&bwriter, mongo->id, 4, 1);//reqid
    binary_set_integer(&bwriter, 0, 4, 1);//respto
    binary_set_integer(&bwriter, OP_MSG, 4, 1);//prot
    binary_set_integer(&bwriter, flag, 4, 1);//flags
    if (0 == kind) {
        binary_set_int8(&bwriter, 0);//kind
    } else {
        binary_set_int8(&bwriter, 1);//kind
        binary_set_integer(&bwriter, 4 + strlen(docid) + 1 + dlens, 4, 1);
        binary_set_string(&bwriter, docid, 0);
    }
    binary_set_string(&bwriter, docs, dlens);//ÕýÎÄ
    *size = bwriter.offset;
    binary_offset(&bwriter, 0);
    binary_set_integer(&bwriter, *size, 4, 1);
    binary_offset(&bwriter, *size);
    return bwriter.data;
}
static char *_mongo_pack_comment(size_t *lens) {
    binary_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_start(&bson);
    bson_append_utf8(&bson, "application", "srey");
    bson_append_utf8(&bson, "os", OS_NAME);
    bson_append_end(&bson);
    *lens = bson.offset;
    return bson.data;
}
void *mongo_pack_hello(mongo_ctx *mongo, int32_t *reqid, size_t *size) {
    size_t clens;
    char *comment;
    binary_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_start(&bson);
    bson_append_int32(&bson, "hello", 1);
    comment = _mongo_pack_comment(&clens);
    bson_append_document(&bson, "comment", comment, clens);
    FREE(comment);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = mongo_pack_msg(mongo, 0, 0, NULL, bson.data, bson.offset, reqid, size);
    FREE(bson.data);
    return data;
}
void *mongo_pack_scram_client_first(mongo_ctx *mongo, const char *method, int32_t *reqid, size_t *size) {
    if (0 == strlen(mongo->user)
        || 0 == strlen(mongo->password)
        || 0 == strlen(mongo->db)
        || NULL != mongo->scram) {
        return NULL;
    }
    mongo->scram = scram_init(method, 1);
    if (NULL == mongo->scram) {
        return NULL;
    }
    binary_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_start(&bson);
    bson_append_int32(&bson, "saslStart", 1);
    bson_append_utf8(&bson, "mechanism", method);
    scram_set_user(mongo->scram, mongo->user);
    char *first_message = scram_first_message(mongo->scram);
    bson_append_binary(&bson, "payload", BSON_SUBTYPE_BINARY, first_message, strlen(first_message));
    FREE(first_message);
    bson_append_int32(&bson, "autoAuthorize", 1);
    binary_ctx options;
    bson_init(&options, NULL, 0);
    bson_append_start(&options);
    bson_append_bool(&options, "skipEmptyExchange", 1);
    bson_append_end(&options);
    bson_append_document(&bson, "options", options.data, options.offset);
    FREE(options.data);
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = mongo_pack_msg(mongo, 0, 0, NULL, bson.data, bson.offset, reqid, size);
    FREE(bson.data);
    return data;
}
void *mongo_pack_scram_client_final(mongo_ctx *mongo, int32_t convid, char *client_final, int32_t *reqid, size_t *size) {
    binary_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_start(&bson);
    bson_append_int32(&bson, "saslContinue", 1);
    bson_append_int32(&bson, "conversationId", convid);
    bson_append_binary(&bson, "payload", BSON_SUBTYPE_BINARY, client_final, strlen(client_final));
    bson_append_utf8(&bson, "$db", mongo->db);
    bson_append_end(&bson);
    void *data = mongo_pack_msg(mongo, 0, 0, NULL, bson.data, bson.offset, reqid, size);
    FREE(bson.data);
    return data;
}
