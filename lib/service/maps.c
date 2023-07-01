#include "service/maps.h"
#define MINICORO_IMPL
#include "service/minicoro.h"
#include "utils.h"
#include "hashmap.h"

#define MAPCO_INIT_CAP 512

static inline uint64_t _map_cosess_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)&(((co_sess_ctx *)item)->sess), sizeof(((co_sess_ctx *)item)->sess));
}
static inline int _map_cosess_compare(const void *a, const void *b, void *ud) {
    return (int)(((co_sess_ctx *)a)->sess - ((co_sess_ctx *)b)->sess);
}
void _map_cosess_add(mapco_ctx *map, struct mco_coro *co, uint64_t sess) {
    co_sess_ctx cosess;
    cosess.co = co;
    cosess.sess = sess;
    ASSERTAB(NULL == hashmap_set(map->coids, &cosess), "session repeat!");
}
void _map_cosess_del(mapco_ctx *map, uint64_t sess) {
    co_sess_ctx key;
    key.sess = sess;
    hashmap_delete(map->coids, &key);
}
int32_t _map_cosess_get(mapco_ctx *map, uint64_t sess, co_sess_ctx *cosess, int32_t del) {
    co_sess_ctx key;
    key.sess = sess;
    co_sess_ctx *tmp = (co_sess_ctx *)hashmap_get(map->coids, &key);
    if (NULL == tmp) {
        return ERR_FAILED;
    }
    *cosess = *tmp;
    if (0 != del) {
        hashmap_delete(map->coids, &key);
    }
    return ERR_OK;
}
static inline uint64_t _map_cotmo_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    return hash((const char *)&(((co_tmo_ctx *)item)->sess), sizeof(((co_tmo_ctx *)item)->sess));
}
static inline int _map_cotmo_compare(const void *a, const void *b, void *ud) {
    return (int)(((co_tmo_ctx *)a)->sess - ((co_tmo_ctx *)b)->sess);
}
void _map_cotmo_add(mapco_ctx *map, uint32_t type, struct mco_coro *co, uint64_t sess) {
    co_tmo_ctx cotmo;
    cotmo.co = co;
    cotmo.sess = sess;
    cotmo.type = type;
    ASSERTAB(NULL == hashmap_set(map->cotmo, &cotmo), "session repeat!");
}
void _map_cotmo_del(mapco_ctx *map, uint64_t sess) {
    co_tmo_ctx key;
    key.sess = sess;
    hashmap_delete(map->cotmo, &key);
}
int32_t _map_cotmo_get(mapco_ctx *map, uint64_t sess, co_tmo_ctx *cotmo) {
    co_tmo_ctx key;
    key.sess = sess;
    co_tmo_ctx *tmp = (co_tmo_ctx *)hashmap_get(map->cotmo, &key);
    if (NULL == tmp) {
        return ERR_FAILED;
    }
    *cotmo = *tmp;
    hashmap_delete(map->cotmo, &key);
    return ERR_OK;
}
void _map_co_init(mapco_ctx *map) {
    map->coids = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                            sizeof(co_sess_ctx), MAPCO_INIT_CAP, 0, 0,
                                            _map_cosess_hash, _map_cosess_compare, NULL, NULL);
    map->cotmo = hashmap_new_with_allocator(_malloc, _realloc, _free,
                                            sizeof(co_tmo_ctx), MAPCO_INIT_CAP, 0, 0,
                                            _map_cotmo_hash, _map_cotmo_compare, NULL, NULL);
}
void _map_co_free(mapco_ctx *map) {
    hashmap_free(map->coids);
    hashmap_free(map->cotmo);
}
