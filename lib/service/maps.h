#ifndef MAPS_H_
#define MAPS_H_

#include "macro.h"
#if WITH_CORO

typedef struct co_sess_ctx {
    struct mco_coro *co;
    uint64_t sess;
}co_sess_ctx;
typedef struct co_tmo_ctx {
    uint32_t type;
    struct mco_coro *co;
    uint64_t sess;
}co_tmo_ctx;
typedef struct mapco_ctx {
    struct hashmap *coids;
    struct hashmap *cotmo;
}mapco_ctx;

void _map_cosess_add(mapco_ctx *map, struct mco_coro *co, uint64_t sess);
void _map_cosess_del(mapco_ctx *map, uint64_t sess);
int32_t _map_cosess_get(mapco_ctx *map, uint64_t sess, co_sess_ctx *cosess, int32_t del);
//co_tmo_ctx MSG_TYPE_TIMEOUT
void _map_cotmo_add(mapco_ctx *map, uint32_t type, struct mco_coro *co, uint64_t sess);
void _map_cotmo_del(mapco_ctx *map, uint64_t sess);
int32_t _map_cotmo_get(mapco_ctx *map, uint64_t sess, co_tmo_ctx *cotmo);

void _map_co_init(mapco_ctx *map);
void _map_co_free(mapco_ctx *map);

#endif
#endif//MAPS_H_
