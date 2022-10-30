#include "map.h"

struct bucket_ctx 
{
    uint64_t hash : 48;
    uint64_t dib : 16;
};
struct map_ctx
{
    size_t elsize;
    size_t cap;
    uint64_t(*hash)(void *);
    int32_t(*compare)(void *, void *, void *);
    void *udata;
    size_t bucketsz;
    size_t nbuckets;
    size_t count;
    size_t mask;
    size_t growat;
    void *buckets;
    void *spare;
    void *edata;
};
static inline struct bucket_ctx *_bucket_at(struct map_ctx *pmap, size_t uindex) 
{
    return (struct bucket_ctx *)(((char *)pmap->buckets) + (pmap->bucketsz * uindex));
}
static inline void *_bucket_item(struct bucket_ctx *pentry)
{
    return ((char *)pentry) + sizeof(struct bucket_ctx);
}
static inline uint64_t _get_hash(struct map_ctx *pmap, void *pkey)
{
    return pmap->hash(pkey) << 16 >> 16;
}
static inline struct map_ctx *_map_new(size_t uielsize, size_t uicap, uint64_t(*hash)(void *),
    int32_t(*compare)(void *a, void *b, void *pudata), void *pudata)
{
    uicap = ROUND_UP(uicap, ONEK);
    size_t uibucketsz = sizeof(struct bucket_ctx) + uielsize;
    while (uibucketsz & (sizeof(uintptr_t) - 1))
    {
        uibucketsz++;
    }
    struct map_ctx *pmap = MALLOC(sizeof(struct map_ctx) + uibucketsz * 2);
    ASSERTAB(NULL != pmap, ERRSTR_MEMORY);
    pmap->count = 0;
    pmap->elsize = uielsize;
    pmap->bucketsz = uibucketsz;
    pmap->hash = hash;
    pmap->compare = compare;
    pmap->udata = pudata;
    pmap->spare = (char*)pmap + sizeof(struct map_ctx);
    pmap->edata = (char*)pmap->spare + uibucketsz;
    pmap->cap = uicap;
    pmap->nbuckets = uicap;
    pmap->mask = pmap->nbuckets - 1;
    pmap->buckets = CALLOC(pmap->bucketsz * pmap->nbuckets, 1);
    ASSERTAB(NULL != pmap->buckets, ERRSTR_MEMORY);
    pmap->growat = (size_t)(pmap->nbuckets * 0.75);

    return pmap;
}
struct map_ctx *map_new(size_t uielsize, uint64_t(*hash)(void *),
    int32_t(*compare)(void *a, void *b, void *pudata), void *pudata)
{
    return _map_new(uielsize, ONEK, hash, compare, pudata);
}
void map_free(struct map_ctx *pmap)
{
    FREE(pmap->buckets);
    FREE(pmap);
}
static inline void _map_expand(struct map_ctx *pmap, size_t uinewcap) 
{
    size_t j;
    struct bucket_ctx *pentry, *pbucket;
    struct map_ctx *pnew = _map_new(pmap->elsize, uinewcap, pmap->hash, pmap->compare, pmap->udata);
    for (size_t i = 0; i < pmap->nbuckets; i++) 
    {
        pentry = _bucket_at(pmap, i);
        if (0 == pentry->dib) 
        {
            continue;
        }

        pentry->dib = 1;
        j = pentry->hash & pnew->mask;
        for (;;)
        {
            pbucket = _bucket_at(pnew, j);
            if (0 == pbucket->dib) 
            {
                memcpy(pbucket, pentry, pmap->bucketsz);
                break;
            }
            if (pbucket->dib < pentry->dib) 
            {
                memcpy(pnew->spare, pbucket, pmap->bucketsz);
                memcpy(pbucket, pentry, pmap->bucketsz);
                memcpy(pentry, pnew->spare, pmap->bucketsz);
            }
            j = (j + 1) & pnew->mask;
            pentry->dib += 1;
        }
    }

    FREE(pmap->buckets);
    pmap->buckets = pnew->buckets;
    pmap->nbuckets = pnew->nbuckets;
    pmap->mask = pnew->mask;
    pmap->cap = pnew->cap;
    pmap->growat = pnew->growat;
    FREE(pnew);
}
void map_set(struct map_ctx *pmap, void *pitem)
{
    if (pmap->count == pmap->growat)
    {
        _map_expand(pmap, pmap->cap * 2);
    }
    struct bucket_ctx *pbucket, *pentry = pmap->edata;
    pentry->hash = _get_hash(pmap, pitem);
    pentry->dib = 1;
    memcpy(_bucket_item(pentry), pitem, pmap->elsize);
    size_t i = pentry->hash & pmap->mask;

    for (;;)
    {
        pbucket = _bucket_at(pmap, i);
        if (0 == pbucket->dib)
        {
            memcpy(pbucket, pentry, pmap->bucketsz);
            pmap->count++;
            return;
        }
        if (pentry->hash == pbucket->hash
            && ERR_OK == pmap->compare(_bucket_item(pentry), _bucket_item(pbucket), pmap->udata))
        {
            memcpy(_bucket_item(pbucket), _bucket_item(pentry), pmap->elsize);
            return;
        }
        if (pbucket->dib < pentry->dib)
        {
            memcpy(pmap->spare, pbucket, pmap->bucketsz);
            memcpy(pbucket, pentry, pmap->bucketsz);
            memcpy(pentry, pmap->spare, pmap->bucketsz);
        }
        i = (i + 1) & pmap->mask;
        pentry->dib += 1;
    }
}
int32_t map_get(struct map_ctx *pmap, void *pkey, void *pitem)
{
    struct bucket_ctx *pbucket;
    uint64_t ulhash = _get_hash(pmap, pkey);
    size_t i = ulhash & pmap->mask;
    for (;;)
    {
        pbucket = _bucket_at(pmap, i);
        if (0 == pbucket->dib)
        {
            return ERR_FAILED;
        }
        if (pbucket->hash == ulhash
            && ERR_OK == pmap->compare(pkey, _bucket_item(pbucket), pmap->udata))
        {
            if (NULL != pitem)
            {
                memcpy(pitem, _bucket_item(pbucket), pmap->elsize);
            }
            return ERR_OK;
        }
        i = (i + 1) & pmap->mask;
    }
}
int32_t map_remove(struct map_ctx *pmap, void *pkey, void *pitem)
{
    struct bucket_ctx *pbucket, *prev;
    uint64_t ulhash = _get_hash(pmap, pkey);
    size_t i = ulhash & pmap->mask;
    for (;;)
    {
        pbucket = _bucket_at(pmap, i);
        if (0 == pbucket->dib)
        {
            return ERR_FAILED;
        }
        if (pbucket->hash == ulhash
            && ERR_OK == pmap->compare(pkey, _bucket_item(pbucket), pmap->udata))
        {
            if (NULL != pitem)
            {
                memcpy(pitem, _bucket_item(pbucket), pmap->elsize);
            }
            pbucket->dib = 0;
            for (;;)
            {
                prev = pbucket;
                i = (i + 1) & pmap->mask;
                pbucket = _bucket_at(pmap, i);
                if (pbucket->dib <= 1)
                {
                    prev->dib = 0;
                    break;
                }
                memcpy(prev, pbucket, pmap->bucketsz);
                prev->dib--;
            }
            pmap->count--;
            return ERR_OK;
        }
        i = (i + 1) & pmap->mask;
    }
}
void map_clear(struct map_ctx *pmap)
{
    pmap->count = 0;
    ZERO(pmap->buckets, pmap->bucketsz * pmap->cap);
}
size_t map_size(struct map_ctx *pmap)
{
    return pmap->count;
}
void map_iter(struct map_ctx *pmap, int32_t(*iter)(void *pitem, void *pudata), void *pudata)
{
    struct bucket_ctx *pbucket;
    for (size_t i = 0; i < pmap->nbuckets; i++)
    {
        pbucket = _bucket_at(pmap, i);
        if (0 == pbucket->dib)
        {
            continue;
        }
        if (ERR_OK != iter(_bucket_item(pbucket), pudata))
        {
            return;
        }
    }
}
