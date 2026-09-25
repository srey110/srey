#include "protocol/mongo/mongo_parse.h"
#include "serial/bson.h"

// 字段名等于字面量：先比长度，再定长比较
#define KEY_IS(it, lit) ((sizeof(lit) - 1) == (it).keylens && 0 == memcmp((it).key, lit, sizeof(lit) - 1))

// 取 BSON 的 ok 字段(线上是 double)。NaN/Inf/超范围转 int32 各架构结论不同，
// x86 上会得到非 0 值被判成成功，所以取不到合法数值一律当失败
static inline int32_t _mongo_iter_ok(bson_iter *iter) {
    double val = bson_iter_double(iter, NULL);
    if (isnan(val)
        || isinf(val)
        || val < (double)INT32_MIN
        || val > (double)INT32_MAX) {
        return 0;
    }
    return (int32_t)val;
}
int32_t mongo_parse_auth_response(mgopack_ctx *mgopack, int32_t *convid, int32_t *done, char **payload, size_t *plens) {
    int32_t ok = 0;
    *convid = 0;
    *done = 0;
    *payload = NULL;
    *plens = 0;
    bson_ctx bson;
    bson_init(&bson, mgopack->doc, mgopack->dlens);
    bson_iter iter;
    bson_iter_init(&iter, &bson);
    while (bson_iter_next(&iter)) {
        if (KEY_IS(iter, "conversationId")) {
            *convid = bson_iter_int32(&iter, NULL);
        } else if (KEY_IS(iter, "done")) {
            *done = bson_iter_bool(&iter, NULL);
        } else if (KEY_IS(iter, "ok")) {
            ok = _mongo_iter_ok(&iter);
        } else if (KEY_IS(iter, "payload")) {
            *payload = bson_iter_binary(&iter, NULL, plens, NULL);
        }
    }
    if (!ok || NULL == *payload){
        char *errbson = bson_tostring(&bson);
        LOG_WARN("%s", errbson);
        FREE(errbson);
    }
    if (ok && NULL == *payload) {
        return 0;
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
int32_t mongo_parse_check_error(mgopack_ctx *mgpack) {
    bson_ctx bson;
    bson_init(&bson, mgpack->doc, mgpack->dlens);
    bson_iter iter;
    bson_iter_init(&iter, &bson);
    int32_t count = 0;
    int32_t ok = 0, n = 0, writeerrors = 0, writeconcernerror = 0, errmsg = 0, nerrors = 0;
    while (bson_iter_next(&iter)) {
        if (KEY_IS(iter, "ok")) {
            count++;
            ok = _mongo_iter_ok(&iter);
            if (!ok) {
                break;
            }
        } else if (KEY_IS(iter, "n")) {
            count++;
            n = bson_iter_int32(&iter, NULL);
        } else if (KEY_IS(iter, "writeErrors")) {
            count++;
            writeerrors = 1;
        } else if (KEY_IS(iter, "writeConcernError")) {
            count++;
            writeconcernerror = 1;
        } else if (KEY_IS(iter, "errmsg")) {
            count++;
            errmsg = 1;
        } else if (KEY_IS(iter, "nErrors")) {
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
    char *errbson = bson_tostring(&bson);
    LOG_WARN("%s", errbson);
    FREE(errbson);
    return ERR_FAILED;
}
int32_t mongo_parse_startsession(mgopack_ctx *mgpack, char uid[UUID_LENS], int32_t *timeout) {
    *timeout = 0;
    bson_ctx bson;
    bson_init(&bson, mgpack->doc, mgpack->dlens);
    bson_iter iter;
    bson_iter_init(&iter, &bson);
    bson_ctx bsonid;
    bson_iter iterid;
    bson_iter result;
    int32_t ok = 0;
    int32_t hasid = 0;
    while (bson_iter_next(&iter)) {
        if (KEY_IS(iter, "ok")) {
            ok = _mongo_iter_ok(&iter);
            if (!ok) {
                break;
            }
        } else if (KEY_IS(iter, "timeoutMinutes")) {
            *timeout = bson_iter_int32(&iter, NULL);
        } else if (KEY_IS(iter, "id")) {
            if (BSON_DOCUMENT != iter.type) {
                break;
            }
            bson_init(&bsonid, iter.val, iter.lens);
            bson_iter_init(&iterid, &bsonid);
            if (ERR_OK != bson_iter_find(&iterid, "id", &result)) {
                break;
            }
            if (UUID_LENS != result.lens
                || BSON_SUBTYPE_UUID != result.subtype) {
                break;
            }
            memcpy(uid, result.val, result.lens);
            hasid = 1;
        }
    }
    if (!ok || !hasid) {
        char *errbson = bson_tostring(&bson);
        LOG_WARN("%s", errbson);
        FREE(errbson);
        return 0;
    }
    return ok;
}
