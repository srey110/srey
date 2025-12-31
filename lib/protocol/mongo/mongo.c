#include "protocol/mongo/mongo.h"
#include "protocol/mongo/bson.h"
#include "srey/task.h"
#include "utils/binary.h"
#include "crypt/scram.h"
#include "protocol/prots.h"

typedef enum parse_status {
    COMMAND = 0,
    AUTH
}parse_status;

static _handshaked_push _hs_push;

void _mongo_init(void *hspush) {
    _hs_push = hspush;
}
void _mongo_pkfree(void *pack) {
    if (NULL == pack) {
        return;
    }
    mgopack_ctx *mgopack = pack;
    FREE(mgopack->payload);
    FREE(mgopack);
}
void _mongo_udfree(ud_cxt *ud) {
    if (NULL == ud->context) {
        return;
    }
    mongo_ctx *mongo = (mongo_ctx *)ud->context;
    scram_free(mongo->scram);
    mongo->scram = NULL;
    FREE(mongo->error);
    mongo->fd = INVALID_SOCK;
    ud->context = NULL;
}
void _mongo_closed(ud_cxt *ud) {
    _mongo_udfree(ud);
}
static void _mongo_auth_response(mgopack_ctx *mgopack, int32_t *convid, int32_t *ok, int32_t *done,
    char **payload, size_t *plens) {
    bson_ctx bson;
    bson_init(&bson, mgopack->doc, mgopack->dlens);
    bson_iter iter;
    bson_iter_init(&iter, &bson);
    while (bson_iter_next(&iter)) {
        if (0 == strcmp(iter.key, "conversationId")) {
            *convid = bson_iter_int32(&iter, NULL);
        } else if (0 == strcmp(iter.key, "done")) {
            *done = bson_iter_bool(&iter, NULL);
        } else if (0 == strcmp(iter.key, "ok")) {
            *ok = (int32_t)bson_iter_double(&iter, NULL);
        } else if (0 == strcmp(iter.key, "payload")) {
            *payload = bson_iter_binary(&iter, NULL, plens, NULL);
        }
    }
}
static void _mongo_format_pwd(mongo_ctx *mongo, char fmtpwd[HEX_ENSIZE(MD5_BLOCK_SIZE)]) {
    char *buf = format_va("%s:mongo:%s", mongo->user, mongo->password);
    char hs[MD5_BLOCK_SIZE];
    md5_ctx md5;
    md5_init(&md5);
    md5_update(&md5, buf, strlen(buf));
    FREE(buf);
    md5_final(&md5, hs);
    tohex(hs, sizeof(hs), fmtpwd);
    strlower(fmtpwd);
}
static int32_t _mongo_server_first_message(ev_ctx *ev, mongo_ctx *mongo, mgopack_ctx *mgopack) {
    size_t size;
    int32_t convid, done, ok = 0;
    char *payload;
    _mongo_auth_response(mgopack, &convid, &ok, &done, &payload, &size);
    if (!ok) {
        return ERR_FAILED;
    }
    if (ERR_OK != scram_parse_first_message(mongo->scram, payload, size)) {
        return ERR_FAILED;
    }
    char *client_final;
    if (DG_SHA1 == mongo->scram->dtype) {
        char fmtpwd[HEX_ENSIZE(MD5_BLOCK_SIZE)];
        _mongo_format_pwd(mongo, fmtpwd);
        scram_set_pwd(mongo->scram, fmtpwd);
        client_final = scram_final_message(mongo->scram);
    } else {
        scram_set_pwd(mongo->scram, mongo->password);
        client_final = scram_final_message(mongo->scram);
    }
    if (NULL == client_final) {
        return ERR_FAILED;
    }
    void *data = mongo_pack_scram_client_final(mongo, convid, client_final, &size);
    FREE(client_final);
    return ev_send(ev, mongo->fd, mongo->skid, data, size, 0);
}
static int32_t _mongo_server_final_message(ev_ctx *ev, mongo_ctx *mongo, mgopack_ctx *mgopack) {
    size_t plens;
    int32_t convid, done, ok;
    char *payload;
    _mongo_auth_response(mgopack, &convid, &ok, &done, &payload, &plens);
    if (!ok
        || !done) {
        return ERR_FAILED;
    }
    return scram_check_final_message(mongo->scram, payload, plens);
}
static void _mongo_scram_auth(ev_ctx *ev, mgopack_ctx *mgopack, ud_cxt *ud) {
    int32_t rtn;
    mongo_ctx *mongo = ud->context;
    switch (mongo->scram->status) {
    case SCRAM_LOCAL_FIRST:
        rtn = _mongo_server_first_message(ev, mongo, mgopack);
        if (ERR_OK != rtn) {
            ud->status = COMMAND;
            _hs_push(mongo->fd, mongo->skid, 1, ud, rtn, mgopack, 0);
            scram_free(mongo->scram);
            mongo->scram = NULL;
        } else {
            _mongo_pkfree(mgopack);
        }
        break;
    case SCRAM_LOCAL_FINAL:
        ud->status = COMMAND;
        rtn = _mongo_server_final_message(ev, mongo, mgopack);
        if (ERR_OK != rtn) {
            _hs_push(mongo->fd, mongo->skid, 1, ud, rtn, mgopack, 0);
        } else {
            _hs_push(mongo->fd, mongo->skid, 1, ud, rtn, NULL, 0);
            _mongo_pkfree(mgopack);
        }
        scram_free(mongo->scram);
        mongo->scram = NULL;
        break;
    default:
        break;
    }
}
void *mongo_unpack(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t blens = buffer_size(buf);
    if (blens < 4) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    uint32_t total;
    ASSERTAB(sizeof(total) == buffer_copyout(buf, 0, &total, sizeof(total)), "copy buffer failed.");
    total = (uint32_t)unpack_integer((const char *)&total, sizeof(total), 1, 0);
    if (blens < (size_t)total) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    mgopack_ctx *mgopack;
    CALLOC(mgopack, 1, sizeof(mgopack_ctx));
    MALLOC(mgopack->payload, total);
    ASSERTAB(total == buffer_remove(buf, mgopack->payload, total), "copy buffer failed.");
    mgopack->total = total;
    binary_ctx breader;
    binary_init(&breader, mgopack->payload, total, 0);
    binary_get_skip(&breader, 4);
    mgopack->reqid = (int32_t)binary_get_integer(&breader, 4, 1);
    mgopack->respto = (int32_t)binary_get_integer(&breader, 4, 1);
    mgopack->prot = (int32_t)binary_get_integer(&breader, 4, 1);
    if (OP_MSG != mgopack->prot) {
        BIT_SET(*status, PROT_ERROR);
        LOG_WARN("unsupported protocol %d.", mgopack->prot);
        _mongo_pkfree(mgopack);
        return NULL;
    }
    mgopack->flags = (uint32_t)binary_get_integer(&breader, 4, 1);
    if (0 != mgopack->flags
        && MORETOCOME != mgopack->flags) {
        BIT_SET(*status, PROT_ERROR);
        LOG_WARN("unsupported flags %d.", mgopack->flags);
        _mongo_pkfree(mgopack);
        return NULL;
    }
    mgopack->kind = binary_get_int8(&breader);
    switch (mgopack->kind) {
    case 0:
        break;
    case 1:
        mgopack->klens = (uint32_t)binary_get_integer(&breader, 4, 1);
        mgopack->docid = binary_get_string(&breader, 0);
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        LOG_WARN("unsupported OP_MSG reply: %d section", mgopack->kind);
        _mongo_pkfree(mgopack);
        return NULL;
    }
    mgopack->dlens = (uint32_t)(breader.size - breader.offset);
    mgopack->doc = breader.data + breader.offset;
    switch (ud->status) {
    case COMMAND:
        return mgopack;
    case AUTH:
        _mongo_scram_auth(ev, mgopack, ud);
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        _mongo_pkfree(mgopack);
        return NULL;
    }
    return NULL;
}
void mongo_init(mongo_ctx *mongo, const char *ip, uint16_t port, struct evssl_ctx *evssl, const char *db) {
    ZERO(mongo, sizeof(mongo_ctx));
    mongo->reqid = 1;
    mongo->fd = INVALID_SOCK;
    strcpy(mongo->ip, ip);
    mongo->port = 0 == port ? 27017 : port;
    mongo->evssl = evssl;
    if (EMPTYSTR(db)) {
        strcpy(mongo->db, "admin");
    } else {
        strcpy(mongo->db, db);
    }
}
void mongo_db(mongo_ctx *mongo, const char *db) {
    strcpy(mongo->db, db);
    mongo->collection[0] = '\0';
}
void mongo_authdb(mongo_ctx *mongo, const char *db) {
    strcpy(mongo->authdb, db);
}
void mongo_collection(mongo_ctx *mongo, const char *collection) {
    strcpy(mongo->collection, collection);
}
void mongo_user_pwd(mongo_ctx *mongo, const char *user, const char *pwd) {
    strcpy(mongo->user, user);
    strcpy(mongo->password, pwd);
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
const char *mongo_error(mongo_ctx *mongo) {
    return mongo->error;
}
void mongo_set_error(mongo_ctx *mongo, const char *err, int32_t copy) {
    FREE(mongo->error);
    if (NULL == err) {
        return;
    }
    if (copy) {
        MALLOC(mongo->error, strlen(err) + 1);
        strcpy(mongo->error, err);
    } else {
        mongo->error = (char *)err;
    }
}
int32_t mongo_requestid(mongo_ctx *mongo) {
    return mongo->reqid;
}
void mongo_set_flag(mongo_ctx *mongo, mongo_flags flag) {
    if (MORETOCOME != flag) {
        return;
    }
    BIT_SET(mongo->flags, flag);
}
int32_t mongo_check_flag(mongo_ctx *mongo, mongo_flags flag) {
    return BIT_CHECK(mongo->flags, flag);
}
int32_t mongo_clear_flag(mongo_ctx *mongo) {
    int32_t flags = mongo->flags;
    mongo->flags = 0;
    return flags;
}
int32_t mongo_status_auth(void) {
    return AUTH;
}
