#ifndef MONGO_H_
#define MONGO_H_

#include "srey/spub.h"
#include "protocol/mongo/mongo_pack.h"

void _mongo_init(void *hspush);
void _mongo_pkfree(void *pack);
void _mongo_udfree(ud_cxt *ud);
void _mongo_closed(ud_cxt *ud);
void *mongo_unpack(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status);

//authmod SCRAM-SHA-1 SCRAM-SHA-256
void mongo_init(mongo_ctx *mongo, const char *ip, uint16_t port, struct evssl_ctx *evssl);
void mongo_db(mongo_ctx *mongo, const char *db);
void mongo_collection(mongo_ctx *mongo, const char *collection);
void mongo_user_pwd(mongo_ctx *mongo, const char *user, const char *pwd);
int32_t mongo_status_auth(void);

int32_t mongo_try_connect(task_ctx *task, mongo_ctx *mongo);

#endif//MONGO_H_
