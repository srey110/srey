#ifndef MONGO_H_
#define MONGO_H_

#include "event/event.h"

typedef struct mongo_ctx {
    uint16_t port;
    scram_authmod authmod;
    int32_t id;
    int32_t status;
    SOCKET fd;
    uint64_t skid;
    struct task_ctx *task;
    struct evssl_ctx *evssl;
    char ip[IP_LENS];
    char curdb[64];
    char authdb[64];
    char user[64];
    char password[64];
}mongo_ctx;

void _mongo_init(void *hspush);
void _mongo_pkfree(void *pack);
void _mongo_udfree(ud_cxt *ud);
void _mongo_closed(ud_cxt *ud);
int32_t _mongo_connected(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud);
int32_t _mongo_ssl_exchanged(ev_ctx *ev, ud_cxt *ud);
void *mongo_unpack(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status);
//SCRAM_SHA1 SCRAM_SHA256
void mongo_init(mongo_ctx *mongo, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, scram_authmod authmod, const char *authdb);
void *mongo_pack_msg(mongo_ctx *mongo, const uint8_t *doc, size_t dlens, int32_t *reqid, size_t *size);

#endif//MONGO_H_
