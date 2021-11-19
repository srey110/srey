#ifndef MAP_H_
#define MAP_H_

#include "rwlock.h"

struct map_ctx *map_new(size_t uielsize, uint64_t(*hash)(void *),
    int32_t(*compare)(void *a, void *b, void *pudata), void *pudata);
void map_free(struct map_ctx *pmap);

size_t _map_size(struct map_ctx *pmap);
size_t map_size(struct map_ctx *pmap);

void _map_set(struct map_ctx *pmap, void *pitem);
void map_set(struct map_ctx *pmap, void *pitem);

int32_t _map_get(struct map_ctx *pmap, void *pkey, void *pitem);
int32_t map_get(struct map_ctx *pmap, void *pkey, void *pitem);

int32_t _map_remove(struct map_ctx *pmap, void *pkey, void *pitem);
int32_t map_remove(struct map_ctx *pmap, void *pkey, void *pitem);

void _map_iter(struct map_ctx *pmap, int32_t(*iter)(void *pitem, void *pudata), void *pudata);
void map_iter(struct map_ctx *pmap, int32_t(*iter)(void *pitem, void *pudata), void *pudata);

#endif//MAP_H_
