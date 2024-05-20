#ifndef WEBSOCK_H_
#define WEBSOCK_H_

#include "event/event.h"

typedef enum  websock_proto {
    WBSK_CONTINUE = 0x00,
    WBSK_TEXT = 0x01,
    WBSK_BINARY = 0x02,
    WBSK_CLOSE = 0x08,
    WBSK_PING = 0x09,
    WBSK_PONG = 0x0A
}websock_proto;

struct websock_pack_ctx *websock_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid,
    buffer_ctx *buf, ud_cxt *ud, int32_t *closefd, int32_t *slice);
char *websock_handshake_pack(const char *host);

void websock_ping(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t mask);
void websock_pong(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t mask);
void websock_close(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t mask);
void websock_text(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t mask,
    int32_t fin, void *data, size_t dlens);
void websock_binary(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t mask,
    int32_t fin, void *data, size_t dlens);
void websock_continuation(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t mask,
    int32_t fin, void *data, size_t dlens);

int32_t websock_pack_fin(struct websock_pack_ctx *pack);
int32_t websock_pack_proto(struct websock_pack_ctx *pack);
char *websock_pack_data(struct websock_pack_ctx *pack, size_t *lens);

void _websock_init(void *hspush);

#endif//WEBSOCK_H_
