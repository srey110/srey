#include "containers/sarray.h"

#define ARRAY_INIT_SIZE 32 // 默认初始容量

void array_init(array_ctx *arr, uint32_t elsize, uint32_t maxsize) {
    ASSERTAB(elsize > 0, "elsize invalid.");
    ASSERTAB(maxsize < UINT32_MAX, "maxsize overflow.");
    arr->elsize = elsize;
    arr->size = 0;
    arr->maxsize = (0 == maxsize) ? ARRAY_INIT_SIZE : ROUND_UP(maxsize, 2);
    ASSERTAB((size_t)arr->maxsize <= SIZE_MAX / elsize, "byte size overflow.");
    MALLOC(arr->ptr, (size_t)elsize * arr->maxsize);
}
void array_free(array_ctx *arr) {
    FREE(arr->ptr);
    // 同 queue_free:长度字段留旧值的话,free 后再 push_back 会绕过扩容分支往 NULL 上写
    arr->size = 0;
    arr->maxsize = 0;
}
void array_resize(array_ctx *arr, uint32_t maxsize) {
    ASSERTAB(maxsize < UINT32_MAX, "maxsize overflow.");
    maxsize = (0 == maxsize) ? ARRAY_INIT_SIZE : ROUND_UP(maxsize, 2);
    ASSERTAB(maxsize >= arr->size, "max size must big than element count.");
    ASSERTAB((size_t)maxsize <= SIZE_MAX / arr->elsize, "byte size overflow.");
    REALLOC(arr->ptr, arr->ptr, (size_t)arr->elsize * maxsize);
    arr->maxsize = maxsize;
}
void array_add(array_ctx *arr, const void *elem, int32_t pos) {
    uint32_t p = _array_norm_pos(arr, pos, 1);// 插入位允许等于 size
    if (arr->size == arr->maxsize) {
        ASSERTAB(arr->maxsize <= UINT32_MAX / 2, "array maxsize overflow.");
        array_resize(arr, arr->maxsize * 2);
    }
    if (p < arr->size) {
        memmove((char *)arr->ptr + ((size_t)p + 1) * arr->elsize,
                (char *)arr->ptr + (size_t)p * arr->elsize,
                (size_t)(arr->size - p) * arr->elsize);
    }
    memcpy((char *)arr->ptr + (size_t)p * arr->elsize, elem, arr->elsize);
    arr->size++;
}
void array_del(array_ctx *arr, int32_t pos) {
    uint32_t p = _array_norm_pos(arr, pos, 0);
    arr->size--;
    if (p < arr->size) {
        memmove((char *)arr->ptr + (size_t)p * arr->elsize,
                (char *)arr->ptr + ((size_t)p + 1) * arr->elsize,
                (size_t)(arr->size - p) * arr->elsize);
    }
}
void array_swap(array_ctx *arr, int32_t pos1, int32_t pos2) {
    uint32_t p1 = _array_norm_pos(arr, pos1, 0);
    uint32_t p2 = _array_norm_pos(arr, pos2, 0);
    if (p1 == p2) {
        return;
    }
    char *a = (char *)arr->ptr + (size_t)p1 * arr->elsize;
    char *b = (char *)arr->ptr + (size_t)p2 * arr->elsize;
    char tmp;
    for (uint32_t i = 0; i < arr->elsize; i++) {
        tmp = a[i];
        a[i] = b[i];
        b[i] = tmp;
    }
}
