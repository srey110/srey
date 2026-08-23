#include "containers/queue.h"

#define QUEUE_INIT_SIZE 32 // 默认初始容量

void queue_init(queue_ctx *qu, uint32_t elsize, uint32_t maxsize) {
    ASSERTAB(elsize > 0, "elsize invalid.");
    ASSERTAB(maxsize < UINT32_MAX, "maxsize overflow.");
    qu->elsize = elsize;
    qu->offset = 0;
    qu->size = 0;
    if (0 == maxsize) {
        // 延迟分配：不预付内存，首次 queue_push 命中 size==maxsize 由 queue_resize(0) 按默认容量分配。
        // 供可能永不装入元素的层使用(如 fsqu 的溢出层)
        qu->maxsize = 0;
        qu->ptr = NULL;
        return;
    }
    qu->maxsize = ROUND_UP(maxsize, 2);
    ASSERTAB((size_t)qu->maxsize <= SIZE_MAX / elsize, "byte size overflow.");
    MALLOC(qu->ptr, (size_t)elsize * qu->maxsize);
}
void queue_free(queue_ctx *qu) {
    FREE(qu->ptr);
    // 长度字段一并复位:只置空 ptr 会留下 size < maxsize 的不一致态,
    // 再 push 不触发 resize 而是直接往 NULL 上算偏移写(同 binary_free)
    qu->offset = 0;
    qu->size = 0;
    qu->maxsize = 0;
}
void queue_resize(queue_ctx *qu, uint32_t maxsize) {
    ASSERTAB(maxsize < UINT32_MAX, "maxsize overflow.");
    maxsize = (0 == maxsize) ? QUEUE_INIT_SIZE : ROUND_UP(maxsize, 2);
    ASSERTAB(maxsize >= qu->size, "max size must big than element count.");
    ASSERTAB((size_t)maxsize <= SIZE_MAX / qu->elsize, "byte size overflow.");
    void *pnew;
    MALLOC(pnew, (size_t)qu->elsize * maxsize);
    // 旧缓冲按 offset 环形排列，新缓冲从下标 0 起线性放置；环形最多跨两段，故两次 memcpy 足够。
    // 不逐元素拷：fsqu 的溢出层是在自旋锁内扩容的，那种循环会把锁按元素个数拉长
    if (0 != qu->size) {
        uint32_t first = qu->maxsize - qu->offset;// offset 到缓冲末尾的元素数
        if (first > qu->size) {
            first = qu->size;
        }
        memcpy(pnew, (char *)qu->ptr + (size_t)qu->offset * qu->elsize,
               (size_t)first * qu->elsize);
        if (qu->size > first) {
            memcpy((char *)pnew + (size_t)first * qu->elsize, qu->ptr,
                   (size_t)(qu->size - first) * qu->elsize);
        }
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
        qu->offset = _queue_wrap(qu, qu->offset + 1);
        qu->size--;
        return;
    }
    uint32_t cur, nxt;
    for (uint32_t i = pos; i + 1 < qu->size; i++) {
        cur = _queue_wrap(qu, qu->offset + i);
        nxt = _queue_wrap(qu, qu->offset + i + 1);
        memcpy((char *)qu->ptr + (size_t)cur * qu->elsize,
               (char *)qu->ptr + (size_t)nxt * qu->elsize,
               qu->elsize);
    }
    qu->size--;
}
