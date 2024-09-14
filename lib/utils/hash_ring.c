#include "hash_ring.h"

typedef struct hash_ring_list {
    hash_ring_node *node;
    struct hash_ring_list *next;
} hash_ring_list;
typedef struct hash_ring_item {
    hash_ring_node *node;
    uint64_t digest;
} hash_ring_item;

static int32_t _item_sort(const void *a, const void *b) {
    hash_ring_item *itema = *(hash_ring_item**)a, *itemb = *(hash_ring_item**)b;
    if (NULL == itema) {
        return 1;
    }
    if (NULL == itemb) {
        return -1;
    }
    if (itema->digest < itemb->digest) {
        return -1;
    } else if (itema->digest > itemb->digest) {
        return 1;
    } else {
        return 0;
    }
}
void hash_ring_init(hash_ring_ctx *ring) {
    ZERO(ring, sizeof(hash_ring_ctx));
    digest_init(&ring->md5, DG_MD5);
}
void hash_ring_free(hash_ring_ctx *ring) {
    if (NULL == ring) {
        return;
    }
    hash_ring_list *tmp, *cur = ring->nodes;
    while(NULL != cur) {
        FREE(cur->node->name);
        FREE(cur->node);
        tmp = cur;
        cur = tmp->next;
        FREE(tmp);
    }
    ring->nodes = NULL;
    for(uint32_t i = 0; i < ring->nitems; i++) {
        FREE(ring->items[i]);
    }
    FREE(ring->items);
}
static uint64_t _hash(hash_ring_ctx *ring, void *data, size_t lens) {
    uint8_t digest[DG_BLOCK_SIZE];
    digest_update(&ring->md5, data, lens);
    digest_final(&ring->md5, (char *)digest);
    digest_reset(&ring->md5);
    return (uint32_t)(digest[3] << 24 | digest[2] << 16 | digest[1] << 8 | digest[0]);
}
static void _add_items(hash_ring_ctx *ring, hash_ring_node *node) {
    uint8_t *name;
    char concat_buf[16];
    int32_t concat_len;
    hash_ring_item *item;
    REALLOC(ring->items, ring->items, (sizeof(hash_ring_item *) * (ring->nitems + node->nreplicas)));
    for(uint32_t i = 0; i < node->nreplicas; i++) {
        concat_len = SNPRINTF(concat_buf, sizeof(concat_buf), "-%d", i);
        ASSERTAB(concat_len > 0, "out of memory.");
        MALLOC(name, (size_t)concat_len + node->lens);
        memcpy(name, node->name, node->lens);
        memcpy(name + node->lens, concat_buf, (size_t)concat_len);
        MALLOC(item, sizeof(hash_ring_item));
        item->node = node;
        item->digest = _hash(ring, name, (size_t)concat_len + node->lens);
        ring->items[ring->nitems + i] = item;
        FREE(name);
    }
    ring->nitems += node->nreplicas;
}
int32_t hash_ring_add_node(hash_ring_ctx *ring, void *name, size_t lens, uint32_t nreplicas) {
    if (NULL == ring
        || NULL == name
        || 0 == lens
        || 0 == nreplicas) {
        return ERR_FAILED;
    }
    if (NULL != hash_ring_get_node(ring, name, lens)) {
        return ERR_FAILED;
    }
    hash_ring_node *node;
    MALLOC(node, sizeof(hash_ring_node));
    MALLOC(node->name, lens);
    memcpy(node->name, name, lens);
    node->lens = lens;
    node->nreplicas = nreplicas;
    hash_ring_list *cur;
    MALLOC(cur, sizeof(hash_ring_list));
    cur->node = node;
    // Add the node
    hash_ring_list *tmp = ring->nodes;
    ring->nodes = cur;
    ring->nodes->next = tmp;
    ring->nnodes++;
    // Add the items for this node
    _add_items(ring, node);
    // Sort the items
    qsort((void**)ring->items, ring->nitems, sizeof(struct hash_ring_item*), _item_sort);
    return ERR_OK;
}
hash_ring_node *hash_ring_get_node(hash_ring_ctx *ring, void *name, size_t lens) {
    if (NULL == ring
        || NULL == name
        || 0 == lens) {
        return NULL;
    }
    hash_ring_list *cur = ring->nodes;
    while (NULL != cur) {
        if (cur->node->lens == lens
            && 0 == memcmp(cur->node->name, name, lens)) {
            return cur->node;
        }
        cur = cur->next;
    }
    return NULL;
}
void hash_ring_remove_node(hash_ring_ctx *ring, void *name, size_t lens) {
    if (NULL == ring
        || NULL == name
        || 0 == lens) {
        return;
    }
    hash_ring_list *next, *prev = NULL, *cur = ring->nodes;
    while(NULL != cur) {
        if(cur->node->lens == lens
           && 0 == memcmp(cur->node->name, name, lens)) {
            // Node found, remove it
            next = cur->next;
            FREE(cur->node->name);
            if (prev == NULL) {
                ring->nodes = next;
            } else {
                prev->next = next;
            }
            // Remove all items for this node and mark them as NULL
            for (uint32_t i = 0; i < ring->nitems; i++) {
                if (ring->items[i]->node == cur->node) {
                    FREE(ring->items[i]);
                    ring->items[i] = NULL;
                }
            }
            // By re-sorting, all the NULLs will be at the end of the array
            // Then the numItems is reset and that memory is no longer used
            qsort((void**)ring->items, ring->nitems, sizeof(struct hash_ring_item*), _item_sort);
            ring->nitems -= cur->node->nreplicas;
            FREE(cur->node);
            FREE(cur);
            ring->nnodes--;
            return;
        }
        prev = cur;
        cur = prev->next;
    }
}
static hash_ring_item *_find_next_highest_item(hash_ring_ctx *ring, uint64_t digest) {
    if (0 == ring->nitems) {
        return NULL;
    }
    int32_t min = 0;
    int32_t max = ring->nitems - 1, midpointindex;
    hash_ring_item *item = NULL;
    while(1) {
        if(min > max) {
            if(min == ring->nitems) {
                // Past the end of the ring, return the first item
                return ring->items[0];
            } else {
                // Return the next highest item
                return ring->items[min];
            }
        }
        midpointindex = (min + max) / 2;
        item = ring->items[midpointindex];
        if(item->digest > digest) {
            // Key is in the lower half
            max = midpointindex - 1;
        } else if(item->digest <= digest) {
            // Key is in the upper half
            min = midpointindex + 1;
        }
    }
    return NULL;
}
hash_ring_node *hash_ring_find_node(hash_ring_ctx *ring, void *key, size_t lens) {
    if (NULL == ring
        || NULL == key
        || 0 == lens) {
        return NULL;
    }
    uint64_t digest = _hash(ring, key, lens);
    hash_ring_item *item = _find_next_highest_item(ring, digest);
    if(item == NULL) {
        return NULL;
    } else {
        return item->node;
    }
}
void hash_ring_print(hash_ring_ctx *ring) {
    uint32_t x, y;
    printf("----------------------------------------\n");
    printf("hash_ring\n\n");
    printf("Nodes: \n\n");
    hash_ring_list *cur = ring->nodes;
    x = 0;
    uint8_t *name;
    while (cur != NULL) {
        printf("%d: ", x);
        name = cur->node->name;
        for (y = 0; y < cur->node->lens; y++) {
            printf("%c", name[y]);
        }
        printf("\n");
        cur = cur->next;
        x++;
    }
    printf("\n");
    printf("Items (%d): \n\n", ring->nitems);
    for (x = 0; x < ring->nitems; x++) {
        hash_ring_item *item = ring->items[x];
        printf("%" PRIu64 " : ", item->digest);
        name = item->node->name;
        for (y = 0; y < item->node->lens; y++) {
            printf("%c", name[y]);
        }
        printf("\n");
    }
    printf("\n");
    printf("----------------------------------------\n");
}
