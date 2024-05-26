#ifndef MYSQL_H_
#define MYSQL_H_

#include "event/event.h"
#include "srey/spub.h"

typedef enum mysql_proto {
    MYSQL_OK = 0x00,
    MYSQL_ERR,
    MYSQL_EOF,
    MYSQL_DATA
}mysql_proto;
typedef struct mysql_pack_ctx {
    int32_t payload_lens;
    int32_t sequence_id;
    mysql_proto proto;
    void *payload;
}mysql_pack_ctx;

void mysql_pkfree(void *pack);
void mysql_udfree(ud_cxt *ud);
void _mysql_init(void *hspush);
void *mysql_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid,
    buffer_ctx *buf, ud_cxt *ud, int32_t *closefd, int32_t *slice);
//SOCKET mysql_connect(task_ctx *task, const char *ip, uint16_t port,
//    const char *user, const char *password, const char *database, const char *charset, uint64_t *skid);

#endif//MYSQL_H_
