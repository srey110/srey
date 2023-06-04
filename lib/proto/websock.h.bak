#ifndef WEBSOCK_H_
#define WEBSOCK_H_

#include "event/event.h"

void *websock_unpack(ev_ctx *ev, SOCKET fd, buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd);
int32_t websock_client_reqhs(ev_ctx *ev, SOCKET fd, ud_cxt *ud);

void websock_ping(ev_ctx *ev, SOCKET fd);
void websock_pong(ev_ctx *ev, SOCKET fd);
void websock_close(ev_ctx *ev, SOCKET fd);
void websock_text(ev_ctx *ev, SOCKET fd, char key[4], const char *data);
void websock_binary(ev_ctx *ev, SOCKET fd, char key[4], void *data, size_t dlens);
void websock_continuation(ev_ctx *ev, SOCKET fd, int32_t fin, char key[4], void *data, size_t dlens);

int32_t websock_pack_fin(void *data);
int32_t websock_pack_proto(void *data);
char *websock_pack_data(void *data, size_t *lens);

#endif//WEBSOCK_H_
