#include "service/maps.h"
#include "utils.h"
#include "hashmap.h"
#define MINICORO_IMPL
#include "minicoro.h"

static inline uint64_t _map_cosess_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)&(((co_sess_ctx *)item)->session), sizeof(((co_sess_ctx *)item)->session));
}
static inline int _map_cosess_compare(const void *a, const void *b, void *ud) {
    return (int)(((co_sess_ctx *)a)->session - ((co_sess_ctx *)b)->session);
}
void _map_cosess_add(mapco_ctx *map, struct mco_coro *co, uint64_t sess) {
    co_sess_ctx cosess;
    cosess.co = co;
    cosess.session = sess;
    ASSERTAB(NULL == hashmap_set(map->cosess, &cosess), "session repeat!");
}
void _map_cosess_del(mapco_ctx *map, uint64_t sess) {
    co_sess_ctx key;
    key.session = sess;
    hashmap_delete(map->cosess, &key);
}
int32_t _map_cosess_get(mapco_ctx *map, uint64_t sess, co_sess_ctx *cosess) {
    co_sess_ctx key;
    key.session = sess;
    co_sess_ctx *tmp = (co_sess_ctx *)hashmap_get(map->cosess, &key);
    if (NULL == tmp) {
        return ERR_FAILED;
    }
    *cosess = *tmp;
    hashmap_delete(map->cosess, &key);
    return ERR_OK;
}
static inline uint64_t _map_cosk_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)&(((co_sock_ctx *)item)->fd), sizeof(((co_sock_ctx *)item)->fd));
}
static inline int _map_cosk_compare(const void *a, const void *b, void *ud) {
    return (int)(((co_sock_ctx *)a)->fd - ((co_sock_ctx *)b)->fd);
}
void _map_cosk_add(mapco_ctx *map, uint64_t sess, struct mco_coro *co, SOCKET fd) {
    co_sock_ctx cofd;
    cofd.co.co = co;
    cofd.co.session = sess;
    cofd.fd = fd;
    ASSERTAB(NULL == hashmap_set(map->cosk, &cofd), "fd repeat!");
}
void _map_cosk_del(mapco_ctx *map, SOCKET fd) {
    co_sock_ctx key;
    key.fd = fd;
    hashmap_delete(map->cosk, &key);
}
int32_t _map_cosk_get(mapco_ctx *map, SOCKET fd, co_sock_ctx *cofd) {
    co_sock_ctx key;
    key.fd = fd;
    co_sock_ctx *tmp = (co_sock_ctx *)hashmap_get(map->cosk, &key);
    if (NULL == tmp) {
        return ERR_FAILED;
    }
    *cofd = *tmp;
    hashmap_delete(map->cosk, &key);
    return ERR_OK;
}
static inline uint64_t _map_cotmo_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)&(((co_tmo_ctx *)item)->co.session), sizeof(((co_tmo_ctx *)item)->co.session));
}
static inline int _map_cotmo_compare(const void *a, const void *b, void *ud) {
    return (int)(((co_tmo_ctx *)a)->co.session - ((co_tmo_ctx *)b)->co.session);
}
void _map_cotmo_add(mapco_ctx *map, co_tmo_ctx *cotmo) {
    ASSERTAB(NULL == hashmap_set(map->cotmo, cotmo), "fd repeat!");
}
void _map_cotmo_del(mapco_ctx *map, uint64_t sess) {
    co_tmo_ctx key;
    key.co.session = sess;
    hashmap_delete(map->cotmo, &key);
}
int32_t _map_cotmo_get(mapco_ctx *map, uint64_t sess, co_tmo_ctx *cotmo) {
    co_tmo_ctx key;
    key.co.session = sess;
    co_tmo_ctx *tmp = (co_tmo_ctx *)hashmap_get(map->cotmo, &key);
    if (NULL == tmp) {
        return ERR_FAILED;
    }
    *cotmo = *tmp;
    hashmap_delete(map->cotmo, &key);
    return ERR_OK;
}
void _map_co_init(mapco_ctx *map) {
    map->cosess = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                             sizeof(co_sess_ctx), 0, 0, 0,
                                             _map_cosess_hash, _map_cosess_compare, NULL, NULL);
    map->cosk = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                           sizeof(co_sock_ctx), 0, 0, 0,
                                           _map_cosk_hash, _map_cosk_compare, NULL, NULL);
    map->cotmo = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                            sizeof(co_tmo_ctx), 0, 0, 0,
                                            _map_cotmo_hash, _map_cotmo_compare, NULL, NULL);
}
void _map_co_free(mapco_ctx *map) {
    hashmap_free(map->cosess);
    hashmap_free(map->cosk);
    hashmap_free(map->cotmo);
}
