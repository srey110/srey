#include "protocol/mongo/mongo_parse.h"
#include "protocol/mongo/bson.h"

int32_t mongo_parse_auth_response(mgopack_ctx *mgopack, int32_t *convid, int32_t *done, char **payload, size_t *plens) {
    int32_t ok = 0;
    bson_ctx bson;
    bson_init(&bson, mgopack->doc, mgopack->dlens);
    bson_iter iter;
    bson_iter_init(&iter, &bson);
    while (bson_iter_next(&iter)) {
        if (0 == strcmp(iter.key, "conversationId")) {
            *convid = bson_iter_int32(&iter, NULL);
        }
        else if (0 == strcmp(iter.key, "done")) {
            *done = bson_iter_bool(&iter, NULL);
        }
        else if (0 == strcmp(iter.key, "ok")) {
            ok = (int32_t)bson_iter_double(&iter, NULL);
        }
        else if (0 == strcmp(iter.key, "payload")) {
            *payload = bson_iter_binary(&iter, NULL, plens, NULL);
        }
    }
    return ok;
}
int64_t mongo_cursorid(mgopack_ctx *mgpack) {
    bson_ctx bson;
    bson_init(&bson, mgpack->doc, mgpack->dlens);
    bson_iter iter;
    bson_iter_init(&iter, &bson);
    bson_iter cursorid;
    if (ERR_OK == bson_iter_find(&iter, "cursor.id", &cursorid)) {
        return bson_iter_int64(&cursorid, NULL);
    }
    return 0;
}
int32_t mongo_parse_check_error(mongo_ctx *mongo, mgopack_ctx *mgpack) {
    bson_ctx bson;
    bson_init(&bson, mgpack->doc, mgpack->dlens);
    bson_iter iter;
    bson_iter_init(&iter, &bson);
    int32_t count = 0;
    int32_t ok = 0, n = 0, writeerrors = 0, writeconcernerror = 0, errmsg = 0, nerrors = 0;
    while (bson_iter_next(&iter)) {
        if (0 == strcmp(iter.key, "ok")) {
            count++;
            ok = (int32_t)bson_iter_double(&iter, NULL);
            if (!ok) {
                break;
            }
        }
        else if (0 == strcmp(iter.key, "n")) {
            count++;
            n = bson_iter_int32(&iter, NULL);
        }
        else if (0 == strcmp(iter.key, "writeErrors")) {
            count++;
            writeerrors = 1;
        }
        else if (0 == strcmp(iter.key, "writeConcernError")) {
            count++;
            writeconcernerror = 1;
        }
        else if (0 == strcmp(iter.key, "errmsg")) {
            count++;
            errmsg = 1;
        }
        else if (0 == strcmp(iter.key, "nErrors")) {
            count++;
            nerrors = bson_iter_int32(&iter, NULL);
        }
        if (count >= 6) {
            break;
        }
    }
    if (ok && !writeerrors && !writeconcernerror && !errmsg && !nerrors) {
        return n;
    }
    mongo_set_error(mongo, bson_tostring(&bson), 0);
    return ERR_FAILED;
}
int32_t mongo_parse_startsession(mongo_ctx *mongo, mgopack_ctx *mgpack, char uid[UUID_LENS], int32_t *timeout) {
    bson_ctx bson;
    bson_init(&bson, mgpack->doc, mgpack->dlens);
    bson_iter iter;
    bson_iter_init(&iter, &bson);
    int32_t ok = 0;
    while (bson_iter_next(&iter)) {
        if (0 == strcmp(iter.key, "ok")) {
            ok = (int32_t)bson_iter_double(&iter, NULL);
            if (!ok) {
                break;
            }
        }
        else if (0 == strcmp(iter.key, "timeoutMinutes")) {
            *timeout = bson_iter_int32(&iter, NULL);
        }
        else if (0 == strcmp(iter.key, "id")) {
            if (BSON_DOCUMENT != iter.type) {
                return ERR_FAILED;
            }
            bson_ctx bsonid;
            bson_init(&bsonid, iter.val, iter.lens);
            bson_iter iterid;
            bson_iter_init(&iterid, &bsonid);
            bson_iter result;
            if (ERR_OK != bson_iter_find(&iterid, "id", &result)) {
                return ERR_FAILED;
            }
            if (UUID_LENS != result.lens
                || BSON_SUBTYPE_UUID != result.subtype) {
                return ERR_FAILED;
            }
            memcpy(uid, result.val, result.lens);
        }
    }
    if (!ok) {
        mongo_set_error(mongo, bson_tostring(&bson), 0);
    }
    return ok;
}
