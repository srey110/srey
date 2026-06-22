#include "containers/queue.h"

#define QUEUE_INIT_SIZE 32 // 默认初始容量

void queue_init(queue_ctx *qu, uint32_t elsize, uint32_t maxsize) {
    ASSERTAB(elsize > 0, "elsize invalid.");
    ASSERTAB(maxsize < UINT32_MAX, "maxsize overflow.");
    qu->elsize = elsize;
    qu->offset = 0;
    qu->size = 0;
    qu->maxsize = (0 == maxsize) ? QUEUE_INIT_SIZE : maxsize;
    ASSERTAB((size_t)qu->maxsize <= SIZE_MAX / elsize, "byte size overflow.");
    MALLOC(qu->ptr, (size_t)elsize * qu->maxsize);
}
void queue_free(queue_ctx *qu) {
    FREE(qu->ptr);
}
void queue_resize(queue_ctx *qu, uint32_t maxsize) {
    ASSERTAB(maxsize < UINT32_MAX, "maxsize overflow.");
    maxsize = (0 == maxsize) ? QUEUE_INIT_SIZE : ROUND_UP(maxsize, 2);
    ASSERTAB(maxsize >= qu->size, "max size must big than element count.");
    ASSERTAB((size_t)maxsize <= SIZE_MAX / qu->elsize, "byte size overflow.");
    void *pnew;
    MALLOC(pnew, (size_t)qu->elsize * maxsize);
    // 旧缓冲按 offset 环形排列，新缓冲从下标 0 起线性放置
    uint32_t cur;
    for (uint32_t i = 0; i < qu->size; i++) {
        cur = qu->offset + i;
        if (cur >= qu->maxsize) {
            cur -= qu->maxsize;
        }
        memcpy((char *)pnew + (size_t)i * qu->elsize,
               (char *)qu->ptr + (size_t)cur * qu->elsize,
               qu->elsize);
    }
    FREE(qu->ptr);
    qu->ptr = pnew;
    qu->offset = 0;
    qu->maxsize = maxsize;
}
void queue_del_at(queue_ctx *qu, uint32_t pos) {
    if (pos >= qu->size) {
        return;
    }
    if (0 == pos) {
        qu->offset++;
        if (qu->offset >= qu->maxsize) {
            qu->offset -= qu->maxsize;
        }
        qu->size--;
        return;
    }
    uint32_t cur, nxt;
    for (uint32_t i = pos; i + 1 < qu->size; i++) {
        cur = qu->offset + i;
        if (cur >= qu->maxsize) {
            cur -= qu->maxsize;
        }
        nxt = qu->offset + i + 1;
        if (nxt >= qu->maxsize) {
            nxt -= qu->maxsize;
        }
        memcpy((char *)qu->ptr + (size_t)cur * qu->elsize,
               (char *)qu->ptr + (size_t)nxt * qu->elsize,
               qu->elsize);
    }
    qu->size--;
}
