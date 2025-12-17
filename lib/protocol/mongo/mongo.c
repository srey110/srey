#include "protocol/mongo/mongo.h"
#include "protocol/prots.h"

typedef enum parse_status {
    INIT = 0,
    AUTH,
    COMMAND
}parse_status;

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
    mongo->fd = INVALID_SOCK;
    ud->extra = NULL;
}
void _mongo_closed(ud_cxt *ud) {
    _mongo_udfree(ud);
}
int32_t _mongo_on_connected(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud, int32_t err) {
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
    if (OP_MSG != pack->head.prot) {
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
    const char *user, const char *password, const char *authmod, const char *authdb) {
    ZERO(mongo, sizeof(mongo_ctx));
    mongo->id = 1;
    mongo->fd = INVALID_SOCK;
    strcpy(mongo->ip, ip);
    mongo->port = 0 == port ? 27017 : port;
    mongo->evssl = evssl;
    if (!EMPTYSTR(user)) {
        strcpy(mongo->user, user);
    }
    if (!EMPTYSTR(password)) {
        strcpy(mongo->password, password);
    }
    if (!EMPTYSTR(authmod)){
        strcpy(mongo->authmod, authmod);
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
    pack_integer((char *)&msg->head.prot, OP_MSG, 4, 1);
    msg->flags = 0;
    if (NULL != doc
        && 0 != dlens) {
        memcpy(msg->data, doc, dlens);
    }
    return msg;
}
