#include "protocol/mongo/mongo.h"
#include "protocol/prots.h"

typedef enum mongo_client_status {
    LINKING = 0x01,
    AUTHED = 0x02
}mongo_client_status;
typedef enum parse_status {
    INIT = 0,
    AUTH,
    COMMAND
}parse_status;
typedef enum mongo_prot {
    MONGO_COMPRESSED = 2012,
    MONGO_MSG = 2013
}mongo_prot;
typedef enum mongo_flags {
    MF_CHECKSUM = 0x01,
    MF_MORETOCOME = 0x02,
    EXHAUSTALLOWED = 1 << 16,
}mongo_flags;
typedef struct mongo_head {
    int32_t size; // total message size, including this
    int32_t reqid;//id for this message
    int32_t respto;//requestID from the original request(used in responses from the database)
    int32_t prot;//message
}mongo_head;
typedef struct mongo_msg {
    mongo_head head;
    uint32_t flags;//message flags
    char data[0];
}mongo_msg;
static _handshaked_push _hs_push;

void _mongo_init(void *hspush) {
    _hs_push = hspush;
}
void _mongo_pkfree(void *pack) {
    if (NULL == pack) {
        return;
    }
    mongo_ctx *mongo = (mongo_ctx *)pack;
    FREE(mongo);
}
void _mongo_udfree(ud_cxt *ud) {
    if (NULL == ud->extra) {
        return;
    }
    mongo_ctx *mongo = (mongo_ctx *)ud->extra;
    mongo->status = 0;
    ud->extra = NULL;
}
void _mongo_closed(ud_cxt *ud) {
    _mongo_udfree(ud);
}
int32_t _mongo_connected(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud) {
    return ERR_OK;
}
int32_t _mongo_ssl_exchanged(ev_ctx *ev, ud_cxt *ud) {
    return ERR_OK;
}
void *mongo_unpack(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t blens = buffer_size(buf);
    if (blens < 4) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    int32_t total;
    ASSERTAB(sizeof(total) == buffer_copyout(buf, 0, &total, sizeof(total)), "copy buffer failed.");
    total = (int32_t)unpack_integer((const char *)&total, sizeof(total), 1, 0);
    if (blens < total) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    mongo_msg *pack;
    CALLOC(pack, 1, total);
    ASSERTAB(total == buffer_remove(buf, pack, total), "copy buffer failed.");
    pack->head.size = total;
    pack->head.reqid = (int32_t)unpack_integer((const char *)&pack->head.reqid, sizeof(pack->head.reqid), 1, 0);
    pack->head.respto = (int32_t)unpack_integer((const char *)&pack->head.respto, sizeof(pack->head.respto), 1, 0);
    pack->head.prot = (int32_t)unpack_integer((const char *)&pack->head.prot, sizeof(pack->head.prot), 1, 0);
    if (MONGO_MSG != pack->head.prot) {
        BIT_SET(*status, PROT_ERROR);
        FREE(pack);
        return NULL;
    }
    pack->flags = (uint32_t)unpack_integer((const char *)&pack->flags, sizeof(pack->flags), 1, 0);
    if (0 != pack->flags) {
        BIT_SET(*status, PROT_ERROR);
        FREE(pack);
        LOG_WARN("unsupported flags %.", pack->flags);
        return NULL;
    }
    switch (ud->status) {
    case INIT:
        break;
    case AUTH:
        break;
    case COMMAND:
        break;
    }
    return pack;
}
void mongo_init(mongo_ctx *mongo, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, scram_authmod authmod, const char *authdb) {
    ZERO(mongo, sizeof(mongo_ctx));
    mongo->id = 1;
    strcpy(mongo->ip, ip);
    mongo->port = 0 == port ? 27017 : port;
    mongo->evssl = evssl;
    if (!EMPTYSTR(user)) {
        strcpy(mongo->user, user);
    }
    if (!EMPTYSTR(password)) {
        strcpy(mongo->password, password);
    }
    if (SCRAM_SHA1 == authmod
        || SCRAM_SHA256 == authmod) {
        mongo->authmod = authmod;
    } else {
        mongo->authmod = SCRAM_SHA1;
    }
    if (!EMPTYSTR(authdb)) {
        strcpy(mongo->authdb, authdb);
    } else {
        strcpy(mongo->authdb, "authdb");
    }
}
void *mongo_pack_msg(mongo_ctx *mongo, const uint8_t *doc, size_t dlens, int32_t *reqid, size_t *size) {
    (*size) = sizeof(mongo_msg) + dlens;
    mongo_msg *msg;
    MALLOC(msg, (*size));
    pack_integer((char *)&msg->head.size, (*size), 4, 1);
    mongo->id++;
    pack_integer((char *)&msg->head.reqid, mongo->id, 4, 1);
    if (NULL != reqid) {
        *reqid = mongo->id;
    }
    msg->head.respto = 0;
    pack_integer((char *)&msg->head.prot, MONGO_MSG, 4, 1);
    msg->flags = 0;
    if (NULL != doc
        && 0 != dlens) {
        memcpy(msg->data, doc, dlens);
    }
    return msg;
}
