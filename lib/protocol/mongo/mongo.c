#include "protocol/mongo/mongo.h"
#include "srey/task.h"
#include "utils/binary.h"
#include "utils/utils.h"
#include "crypt/scram.h"
#include "protocol/prots.h"

#define MONGO_MAX_PACK_SIZE (64 * 1024 * 1024)  // MongoDB 单包上限 64MB

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
    mongo->sk.fd = INVALID_SOCK;
    ud->context = NULL;
}
void _mongo_closed(ud_cxt *ud) {
    _mongo_udfree(ud);
}
// 格式化 SCRAM-SHA-1 密码：对 "user:mongo:password" 计算 MD5 后转十六进制小写
static void _mongo_format_pwd(mongo_ctx *mongo, char fmtpwd[HEX_ENSIZE(MD5_BLOCK_SIZE)]) {
    char *buf = format_va("%s:mongo:%s", mongo->user, mongo->password);
    size_t blen = strlen(buf);
    char hs[MD5_BLOCK_SIZE];
    md5_ctx md5;
    md5_init(&md5);
    md5_update(&md5, buf, blen);
    secure_zero(buf, blen);
    FREE(buf);
    md5_final(&md5, hs);
    secure_zero(&md5, sizeof(md5));
    tohex(hs, sizeof(hs), fmtpwd);
    strlower(fmtpwd);
    secure_zero(hs, sizeof(hs));
}
// 处理 SCRAM 服务端第一消息：解析服务端 nonce/salt/迭代次数，构造并发送客户端 final 消息
static int32_t _mongo_server_first_message(ev_ctx *ev, mongo_ctx *mongo, mgopack_ctx *mgopack) {
    size_t size;
    int32_t convid, done;
    char *payload;
    int32_t ok = mongo_parse_auth_response(mgopack, &convid, &done, &payload, &size);
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
        scram_set_pwd(mongo->scram, fmtpwd, strlen(fmtpwd));
        secure_zero(fmtpwd, sizeof(fmtpwd));
        client_final = scram_final_message(mongo->scram);
    } else {
        scram_set_pwd(mongo->scram, mongo->password, strlen(mongo->password));
        client_final = scram_final_message(mongo->scram);
    }
    if (NULL == client_final) {
        return ERR_FAILED;
    }
    void *data = mongo_pack_scram_client_final(mongo, convid, client_final, &size);
    FREE(client_final);
    return ev_send(ev, mongo->sk.fd, mongo->sk.skid, data, size, 0);
}
// 处理 SCRAM 服务端最终消息：验证服务端签名，确认认证完成
static int32_t _mongo_server_final_message(mongo_ctx *mongo, mgopack_ctx *mgopack) {
    size_t plens;
    int32_t convid, done;
    char *payload;
    int32_t ok = mongo_parse_auth_response(mgopack, &convid, &done, &payload, &plens);
    if (!ok || !done) {
        return ERR_FAILED;
    }
    return scram_check_final_message(mongo->scram, payload, plens);
}
// SCRAM 认证状态机：根据 scram->status 分发处理服务端第一/最终消息
static void _mongo_scram_auth(ev_ctx *ev, mgopack_ctx *mgopack, ud_cxt *ud) {
    int32_t rtn;
    mongo_ctx *mongo = ud->context;
    switch (mongo->scram->status) {
    case SCRAM_LOCAL_FIRST:
        rtn = _mongo_server_first_message(ev, mongo, mgopack);
        if (ERR_OK != rtn) {
            ud->status = COMMAND;
            _hs_push(mongo->sk.fd, mongo->sk.skid, 1, ud, rtn, NULL, 0);
            scram_free(mongo->scram);
            mongo->scram = NULL;
        }
        break;
    case SCRAM_LOCAL_FINAL:
        ud->status = COMMAND;
        rtn = _mongo_server_final_message(mongo, mgopack);
        _hs_push(mongo->sk.fd, mongo->sk.skid, 1, ud, rtn, NULL, 0);
        scram_free(mongo->scram);
        mongo->scram = NULL;
        break;
    default:
        break;
    }
    _mongo_pkfree(mgopack);
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
    if (total < 26 || total > MONGO_MAX_PACK_SIZE) {
        BIT_SET(*status, PROT_ERROR);
        return NULL;
    }
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
    if (0 != (mgopack->flags & ~(uint32_t)MORETOCOME)) {
        BIT_SET(*status, PROT_ERROR);
        LOG_WARN("unsupported flags 0x%x (wire checksum not supported by this client).", mgopack->flags);
        _mongo_pkfree(mgopack);
        return NULL;
    }
    mgopack->kind = binary_get_int8(&breader);
    // OP_MSG 仅支持单 Section response: 解析完 section header 后,section 长度必须正好覆盖 OP_MSG body 剩余字节
    size_t section_start = breader.offset;
    uint32_t bson_len;
    switch (mgopack->kind) {
    case 0:
        // body section: 单个 BSON doc, length 在前 4 字节
        if (breader.size - breader.offset < 5) {
            BIT_SET(*status, PROT_ERROR);
            LOG_WARN("invalid OP_MSG kind=0 section too short.");
            _mongo_pkfree(mgopack);
            return NULL;
        }
        bson_len = (uint32_t)unpack_integer(breader.data + breader.offset, 4, 1, 0);
        if (bson_len < 5 || (size_t)bson_len > breader.size - breader.offset) {
            BIT_SET(*status, PROT_ERROR);
            LOG_WARN("invalid OP_MSG kind=0 BSON length %u.", bson_len);
            _mongo_pkfree(mgopack);
            return NULL;
        }
        if (section_start + bson_len != breader.size) {
            BIT_SET(*status, PROT_ERROR);
            LOG_WARN("OP_MSG multi-Section response not supported (kind=0).");
            _mongo_pkfree(mgopack);
            return NULL;
        }
        break;
    case 1:
        mgopack->klens = (uint32_t)binary_get_integer(&breader, 4, 1);
        // klens 含自身 4 字节 size + docid C-string + 0+ BSON docs;
        // 异常值（< 5 或超出 OP_MSG body 剩余）视为协议错误
        if (mgopack->klens < 5 ||
            (size_t)(mgopack->klens - 4) > breader.size - breader.offset) {
            BIT_SET(*status, PROT_ERROR);
            LOG_WARN("invalid OP_MSG kind=1 section length %u.", mgopack->klens);
            _mongo_pkfree(mgopack);
            return NULL;
        }
        mgopack->docid = binary_get_string(&breader);
        if (section_start + mgopack->klens != breader.size) {
            BIT_SET(*status, PROT_ERROR);
            LOG_WARN("OP_MSG multi-Section response not supported (kind=1).");
            _mongo_pkfree(mgopack);
            return NULL;
        }
        // klens=5 + docid="\0" 是协议层合法但 BSON section 为空（dlens=0）；
        // 下游 mongo_parse_*/bson_iter_init 无条件读 4 字节 doclens 触发 ASSERTAB abort,
        // 此处提前拒收避免恶意 server 26 字节构造响应远程让客户端进程崩溃
        if (breader.size - breader.offset < 5) {
            BIT_SET(*status, PROT_ERROR);
            LOG_WARN("OP_MSG kind=1 BSON section too short.");
            _mongo_pkfree(mgopack);
            return NULL;
        }
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
    mongo->sk.fd = INVALID_SOCK;
    safe_fill_str(mongo->ip, sizeof(mongo->ip), ip);
    mongo->port = 0 == port ? 27017 : port;
    mongo->evssl = evssl;
    safe_fill_str(mongo->db, sizeof(mongo->db), EMPTYSTR(db) ? "admin" : db);
}
int32_t mongo_try_connect(task_ctx *task, mongo_ctx *mongo) {
    mongo->task = task;
    return task_connect(task, PACK_MONGO, NULL, mongo->ip, mongo->port,
        NULL == mongo->evssl ? NETEV_NONE : NETEV_AUTHSSL, mongo, &mongo->sk.fd, &mongo->sk.skid);
}
void mongo_db(mongo_ctx *mongo, const char *db) {
    safe_fill_str(mongo->db, sizeof(mongo->db), db);
    mongo->collection[0] = '\0';
}
void mongo_authdb(mongo_ctx *mongo, const char *db) {
    safe_fill_str(mongo->authdb, sizeof(mongo->authdb), db);
}
void mongo_collection(mongo_ctx *mongo, const char *collection) {
    safe_fill_str(mongo->collection, sizeof(mongo->collection), collection);
}
void mongo_user_pwd(mongo_ctx *mongo, const char *user, const char *pwd) {
    safe_fill_str(mongo->user, sizeof(mongo->user), user);
    safe_fill_str(mongo->password, sizeof(mongo->password), pwd);
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
