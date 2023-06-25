#ifndef MAPS_H_
#define MAPS_H_

#include "macro.h"

typedef struct co_sess_ctx {
    struct mco_coro *co;
    uint64_t session;
}co_sess_ctx;
typedef struct co_sock_ctx {
    co_sess_ctx co;
    SOCKET fd;
}co_sock_ctx;
typedef struct co_tmo_ctx {
    uint32_t type;
    SOCKET fd;
    co_sess_ctx co;
}co_tmo_ctx;
typedef struct mapco_ctx {
    struct hashmap *cosess;
    struct hashmap *cotmo;
    struct hashmap *cosk;
}mapco_ctx;
//co_sess_ctx MSG_TYPE_CONNECT MSG_TYPE_RESPONSE  MSG_TYPE_WAITSEND
void _map_cosess_add(mapco_ctx *map, struct mco_coro *co, uint64_t sess);
void _map_cosess_del(mapco_ctx *map, uint64_t sess);
int32_t _map_cosess_get(mapco_ctx *map, uint64_t sess, co_sess_ctx *cosess);
//co_sock_ctx MSG_TYPE_RECV MSG_TYPE_RECVFROM MSG_TYPE_CLOSE slice
void _map_cosk_add(mapco_ctx *map, uint64_t sess, struct mco_coro *co, SOCKET fd);
void _map_cosk_del(mapco_ctx *map, SOCKET fd);
int32_t _map_cosk_get(mapco_ctx *map, SOCKET fd, co_sock_ctx *cofd, int32_t del);
//co_tmo_ctx MSG_TYPE_TIMEOUT
void _map_cotmo_add(mapco_ctx *map, co_tmo_ctx *cotmo);
void _map_cotmo_del(mapco_ctx *map, uint64_t sess);
int32_t _map_cotmo_get(mapco_ctx *map, uint64_t sess, co_tmo_ctx *cotmo);

void _map_co_init(mapco_ctx *map);
void _map_co_free(mapco_ctx *map);

#endif//MAPS_H_
