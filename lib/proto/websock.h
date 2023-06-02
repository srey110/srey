#ifndef WEBSOCK_H_
#define WEBSOCK_H_

#include "structs.h"
#include "event/event.h"

void *websock_unpack(buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd);
void *websock_pack(void *data, size_t lens, size_t *size);

int32_t websock_handshake(ev_ctx *ev, SOCKET fd, ud_cxt *ud);

#endif//WEBSOCK_H_
