#include "containers/sarray.h"

#define ARRAY_INIT_SIZE 32 // 默认初始容量

void array_init(array_ctx *arr, uint32_t elsize, uint32_t maxsize) {
    ASSERTAB(elsize > 0, "elsize invalid.");
    ASSERTAB(maxsize < UINT32_MAX, "maxsize overflow.");
    arr->elsize = elsize;
    arr->size = 0;
    arr->maxsize = (0 == maxsize) ? ARRAY_INIT_SIZE : (uint32_t)ROUND_UP(maxsize, 2);
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
    maxsize = (0 == maxsize) ? ARRAY_INIT_SIZE : (uint32_t)ROUND_UP(maxsize, 2);
    ASSERTAB(maxsize >= arr->size, "max size must big than element count.");
    ASSERTAB((size_t)maxsize <= SIZE_MAX / arr->elsize, "byte size overflow.");
    REALLOC(arr->ptr, arr->ptr, (size_t)arr->elsize * maxsize);
    arr->maxsize = maxsize;
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
