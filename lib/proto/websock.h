#ifndef WEBSOCK_H_
#define WEBSOCK_H_

#include "event/event.h"

struct websock_pack_ctx *websock_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid,
    buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd, int32_t *slice);
SOCKET websock_connect(struct task_ctx *task, const char *host, uint16_t port, struct evssl_ctx *evssl, uint64_t *skid);

void websock_ping(ev_ctx *ev, SOCKET fd, uint64_t skid);
void websock_pong(ev_ctx *ev, SOCKET fd, uint64_t skid);
void websock_close(ev_ctx *ev, SOCKET fd, uint64_t skid);
void websock_text(ev_ctx *ev, SOCKET fd, uint64_t skid,
    int32_t fin, char key[4], const char *data, size_t dlens);
void websock_binary(ev_ctx *ev, SOCKET fd, uint64_t skid,
    int32_t fin, char key[4], void *data, size_t dlens);
void websock_continuation(ev_ctx *ev, SOCKET fd, uint64_t skid,
    int32_t fin, char key[4], void *data, size_t dlens);

int32_t websock_pack_fin(struct websock_pack_ctx *pack);
int32_t websock_pack_proto(struct websock_pack_ctx *pack);
char *websock_pack_data(struct websock_pack_ctx *pack, size_t *lens);

#endif//WEBSOCK_H_
