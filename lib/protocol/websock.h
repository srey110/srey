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

void _websock_hsfree(void *data);
struct websock_pack_ctx *websock_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, int32_t *status);

char *websock_handshake_pack(const char *host, const char *secproto);
void *websock_ping(int32_t mask, size_t *size);
void *websock_pong(int32_t mask, size_t *size);
void *websock_close(int32_t mask, size_t *size);
void *websock_text(int32_t mask, int32_t fin, void *data, size_t dlens, size_t *size);
void *websock_binary(int32_t mask, int32_t fin, void *data, size_t dlens, size_t *size);
void *websock_continuation(int32_t mask, int32_t fin, void *data, size_t dlens, size_t *size);

int32_t websock_pack_fin(struct websock_pack_ctx *pack);
int32_t websock_pack_proto(struct websock_pack_ctx *pack);
char *websock_pack_data(struct websock_pack_ctx *pack, size_t *lens);

void _websock_init(void *hspush);

#endif//WEBSOCK_H_
