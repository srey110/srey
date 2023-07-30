/**
 * Copyright 2015 Chris Moos
 * 
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and 
 * limitations under the License.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "algo/hash_ring.h"
#include "algo/sha1.h"
#include "algo/md5.h"

static int item_sort(const void *a, const void *b);

hash_ring_t *hash_ring_create(uint32_t numReplicas, HASH_FUNCTION hash_fn) {  
    // numReplicas must be greater than or equal to 1
    if (numReplicas <= 0
        || (HASH_FUNCTION_MD5 != hash_fn && HASH_FUNCTION_SHA1 != hash_fn)) {
        return NULL;
    }
    hash_ring_t *ring = (hash_ring_t*)malloc(sizeof(hash_ring_t));
    ring->numReplicas = numReplicas;
    ring->nodes = NULL;
    ring->items = NULL;
    ring->numNodes = 0;
    ring->numItems = 0;
    ring->hash_fn = hash_fn;
    ring->mode = HASH_RING_MODE_NORMAL;
    return ring;
}
void hash_ring_free(hash_ring_t *ring) {
    if (NULL == ring) {
        return;
    }
    // Clean up the nodes
    ll_t *tmp, *cur = ring->nodes;
    while(NULL != cur) {
        free(((hash_ring_node_t*)cur->data)->name);
        free(cur->data);
        tmp = cur;
        cur = tmp->next;
        free(tmp);
    }
    ring->nodes = NULL;    
    // Clean up the items
    for(uint32_t x = 0; x < ring->numItems; x++) {
        free(ring->items[x]);
    }
    if(ring->items != NULL) free(ring->items);
    free(ring);
}
static int hash_ring_hash(hash_ring_t *ring, uint8_t *data, size_t dataLen, uint64_t *hash) {
    if(HASH_FUNCTION_MD5 == ring->hash_fn) {
        md5_ctx state;
        uint8_t digest[MD5_BLOCK_SIZE];
        md5_init(&state);
        md5_update(&state, data, dataLen);
        md5_final(&state, digest);
        if(HASH_RING_MODE_LIBMEMCACHED_COMPAT == ring->mode) {
            *hash = (digest[3] << 24 | digest[2] << 16 | digest[1] << 8 | digest[0]);
            return 0;
        } else {
            uint64_t keyInt = (digest[15] << 24 | digest[14] << 16 | digest[13] << 8 | digest[12]);
            keyInt <<= 32;
            keyInt &= 0xffffffff00000000LLU;
            keyInt |= (digest[11] << 24 | digest[10] << 16 | digest[9] << 8 | digest[8]);
            *hash = keyInt;
            return 0;
        }
    } else if(HASH_FUNCTION_SHA1 == ring->hash_fn) {
        sha1_ctx sha1;
        uint8_t digest[SHA1_BLOCK_SIZE];
        sha1_init(&sha1);
        sha1_update(&sha1, data, dataLen);
        sha1_final(&sha1, digest);
        uint64_t keyInt = sha1.state[3];
        keyInt <<= 32;
        keyInt |= sha1.state[4];
        *hash = keyInt;
        return 0;
    } else {
        return -1;
    }
}
void hash_ring_print(hash_ring_t *ring) {
    if (NULL == ring) {
        return;
    }
    uint32_t x, y;   
    printf("----------------------------------------\n");
    printf("hash_ring\n\n");
    printf("numReplicas:%8d\n", ring->numReplicas);
    printf("Nodes: \n\n");
    ll_t *cur = ring->nodes;
    hash_ring_node_t *node;
    x = 0;
    while(NULL != cur) {
        printf("%d: ", x);
        node = (hash_ring_node_t*)cur->data;
        for(y = 0; y < node->nameLen; y++) {
            printf("%c", node->name[y]);
        }
        printf("\n");
        cur = cur->next;
        x++;
    }
    printf("\n");
    printf("Items (%d): \n\n", ring->numItems);
    hash_ring_item_t *item;
    for(x = 0; x < ring->numItems; x++) {
        item = ring->items[x];
        printf("%" PRIu64 " : ", item->number);
        for(y = 0; y < item->node->nameLen; y++) {
            printf("%c", item->node->name[y]);
        }
        printf("\n");
    }
    printf("\n");
    printf("----------------------------------------\n");
}
static int hash_ring_add_items(hash_ring_t *ring, hash_ring_node_t *node) {
    uint32_t x; 
    char concat_buf[8];
    int concat_len;
    uint64_t keyInt;
    // Resize the items array
    void *resized = realloc(ring->items, (sizeof(hash_ring_item_t*) * ring->numNodes * ring->numReplicas));
    if(NULL == resized) {
        return HASH_RING_ERR;
    }
    uint8_t *data;
    hash_ring_item_t *item;
    ring->items = (hash_ring_item_t**)resized;
    for(x = 0; x < ring->numReplicas; x++) {
        if(HASH_RING_MODE_LIBMEMCACHED_COMPAT == ring->mode) {
            concat_len = snprintf(concat_buf, sizeof(concat_buf), "-%d", x);
        } else {
            concat_len = snprintf(concat_buf, sizeof(concat_buf), "%d", x);
        }
        data = (uint8_t*)malloc(concat_len + node->nameLen);
        memcpy(data, node->name, node->nameLen);
        memcpy(data + node->nameLen, &concat_buf, concat_len);
        if(-1 == hash_ring_hash(ring, data, concat_len + node->nameLen, &keyInt)) {
            free(data);
            return HASH_RING_ERR;
        }
        free(data);
        item = (hash_ring_item_t*)malloc(sizeof(hash_ring_item_t));
        item->node = node;
        item->number = keyInt;
        ring->items[(ring->numNodes - 1) * ring->numReplicas + x] = item;
    }
    ring->numItems += ring->numReplicas;
    return HASH_RING_OK;
}
static int item_sort(const void *a, const void *b) {
    hash_ring_item_t *itemA = *(hash_ring_item_t**)a, *itemB = *(hash_ring_item_t**)b;
    if (NULL == itemA) {
        return 1;
    }
    if (NULL == itemB) {
        return -1;
    }
    if(itemA->number < itemB->number) {
        return -1;
    } else if(itemA->number > itemB->number) {
        return 1;
    } else {
        return 0;
    }
}
int hash_ring_add_node(hash_ring_t *ring, unsigned char *name, size_t nameLen) {
    if (NULL == ring) {
        return HASH_RING_ERR;
    }
    if (NULL != hash_ring_get_node(ring, name, nameLen)) {
        return HASH_RING_ERR;
    }
    if (NULL == name 
        || nameLen <= 0) {
        return HASH_RING_ERR;
    }
    hash_ring_node_t *node = (hash_ring_node_t*)malloc(sizeof(hash_ring_node_t));
    if(NULL == node) {
        return HASH_RING_ERR;
    }
    node->name = (uint8_t*)malloc(nameLen);
    if(NULL == node->name) {
        free(node);
        return HASH_RING_ERR;
    }
    memcpy(node->name, name, nameLen);
    node->nameLen = nameLen;
    ll_t *cur = (ll_t*)malloc(sizeof(ll_t));
    if(NULL == cur) {
        free(node->name);
        free(node);
        return HASH_RING_ERR;
    }
    cur->data = node;
    // Add the node
    ll_t *tmp = ring->nodes;
    ring->nodes = cur;
    ring->nodes->next = tmp;
    ring->numNodes++;
    // Add the items for this node
    if(HASH_RING_OK != hash_ring_add_items(ring, node)) {
        hash_ring_remove_node(ring, node->name, node->nameLen);
        return HASH_RING_ERR;
    }
    // Sort the items
    qsort((void**)ring->items, ring->numItems, sizeof(struct hash_ring_item_t*), item_sort);
    return HASH_RING_OK;
}
int hash_ring_remove_node(hash_ring_t *ring, unsigned char *name, size_t nameLen) {
    if (NULL == ring
        || NULL == name
        || nameLen <= 0) {
        return HASH_RING_ERR;
    }
    hash_ring_node_t *node;
    ll_t *next, *prev = NULL, *cur = ring->nodes;
    uint32_t x;
    while(NULL != cur) {
        node = (hash_ring_node_t*)cur->data;
        if(node->nameLen == nameLen
            && 0 == memcmp(node->name, name, nameLen)) {
                // Node found, remove it
                next = cur->next;
                free(node->name);
                if(NULL == prev) {
                    ring->nodes = next;
                } else {
                    prev->next = next;
                }
                // Remove all items for this node and mark them as NULL
                for(x = 0; x < ring->numItems; x++) {
                    if(ring->items[x]->node == node) {
                        free(ring->items[x]);
                        ring->items[x] = NULL;
                    }
                }
                // By re-sorting, all the NULLs will be at the end of the array
                // Then the numItems is reset and that memory is no longer used
                qsort((void**)ring->items, ring->numItems, sizeof(struct hash_ring_item_t*), item_sort);
                ring->numItems -= ring->numReplicas;
                free(node);
                free(cur);
                ring->numNodes--;
                return HASH_RING_OK;
        }
        prev = cur;
        cur = prev->next;
    }
    return HASH_RING_ERR;
}
hash_ring_node_t *hash_ring_get_node(hash_ring_t *ring, unsigned char *name, size_t nameLen) {
    if (NULL == ring
        || NULL == name
        || nameLen <= 0) {
        return NULL;
    }
    ll_t *cur = ring->nodes;
    hash_ring_node_t *node;
    while(NULL != cur) {
        node = (hash_ring_node_t*)cur->data;
        // Check if the nameLen is the same as well as the name
        if(node->nameLen == nameLen
            && 0 == memcmp(node->name, name, nameLen)) {
            return node;
        }
        cur = cur->next;
    }
    return NULL;
}
hash_ring_item_t *hash_ring_find_next_highest_item(hash_ring_t *ring, uint64_t num) {
    if (0 == ring->numItems) {
        return NULL;
    }    
    int min = 0;
    int midpointIndex;
    int max = ring->numItems - 1;
    hash_ring_item_t *item = NULL;
    while(1) {
        if(min > max) {
            if(min == ring->numItems) {
                // Past the end of the ring, return the first item
                return ring->items[0];
            } else {
                // Return the next highest item
                return ring->items[min];
            }
        }
        midpointIndex = (min + max) / 2;
        item = ring->items[midpointIndex];

        if(item->number > num) {
            // Key is in the lower half
            max = midpointIndex - 1;
        } else if(item->number <= num) {
            // Key is in the upper half
            min = midpointIndex + 1;
        }
    }
    return NULL;
}
hash_ring_node_t *hash_ring_find_node(hash_ring_t *ring, unsigned char *key, size_t keyLen) {
    if (NULL == ring
        || NULL == key
        || keyLen <= 0) {
        return NULL;
    }    
    uint64_t keyInt;    
    if (-1 == hash_ring_hash(ring, key, keyLen, &keyInt)) {
        return NULL;
    }
    hash_ring_item_t *item = hash_ring_find_next_highest_item(ring, keyInt);
    if(NULL == item) {
        return NULL;
    }  else {
        return item->node;
    }
}
/*
 * Consistently hash the key to num nodes;
 * returns the number of nodes found, or -1 if there is an error
 */
int hash_ring_find_nodes(hash_ring_t *ring, unsigned char *key, size_t keyLen, hash_ring_node_t *nodes[], uint32_t num) {
    // the number of nodes we're going to return is either the number of nodes
    // requested, or the number of nodes available
    int ret = ring->numNodes < num ? ring->numNodes : num;
    uint64_t keyInt;
    if (-1 == hash_ring_hash(ring, key, keyLen, &keyInt)) {
        return -1;
    }
    int x = 0;
    int seen;
    int i;
    hash_ring_item_t *item;
    while(1) {
        item = hash_ring_find_next_highest_item(ring, keyInt);
        if (NULL == item) {
            return -1;
        }
        keyInt = item->number;
        // if we've already included this node, skip it
        seen = 0;
        for(i=0; i<x; i++) {
            if(item->node == nodes[i]) {
                seen = 1;
                break;
            }
        }
        if (seen) {
            continue;
        }
        nodes[x] = item->node;
        x++;
        if (x == ret) {
            break;
        }
    }
    return ret;
}
int hash_ring_set_mode(hash_ring_t *ring, HASH_MODE mode) {
    if (NULL == ring) {
        return HASH_RING_ERR;
    }
    if(HASH_RING_MODE_LIBMEMCACHED_COMPAT == mode) {
        if (HASH_FUNCTION_MD5 != ring->hash_fn) {
            return HASH_RING_ERR;
        }
        ring->mode = mode;
        return HASH_RING_OK;
    } else if(HASH_RING_MODE_NORMAL == mode) {
        ring->mode = mode;
        return HASH_RING_OK;
    } else {
        /* Invalid mode */
        return HASH_RING_ERR;
    }
}
