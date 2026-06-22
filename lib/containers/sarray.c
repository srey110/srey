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
    MALLOC(arr->tmp, elsize);
}
void array_free(array_ctx *arr) {
    FREE(arr->ptr);
    FREE(arr->tmp);
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
    if (pos < 0) {
        pos += (int32_t)arr->size;
    }
    ASSERTAB(pos >= 0 && (uint32_t)pos <= arr->size, "pos error.");
    if (arr->size == arr->maxsize) {
        ASSERTAB(arr->maxsize <= UINT32_MAX / 2, "array maxsize overflow.");
        array_resize(arr, arr->maxsize * 2);
    }
    if ((uint32_t)pos < arr->size) {
        memmove((char *)arr->ptr + ((size_t)pos + 1) * arr->elsize,
                (char *)arr->ptr + (size_t)pos * arr->elsize,
                (size_t)(arr->size - (uint32_t)pos) * arr->elsize);
    }
    memcpy((char *)arr->ptr + (size_t)pos * arr->elsize, elem, arr->elsize);
    arr->size++;
}
void array_del(array_ctx *arr, int32_t pos) {
    if (pos < 0) {
        pos += (int32_t)arr->size;
    }
    ASSERTAB(pos >= 0 && (uint32_t)pos < arr->size, "pos error.");
    arr->size--;
    if ((uint32_t)pos < arr->size) {
        memmove((char *)arr->ptr + (size_t)pos * arr->elsize,
                (char *)arr->ptr + ((size_t)pos + 1) * arr->elsize,
                (size_t)(arr->size - (uint32_t)pos) * arr->elsize);
    }
}
void array_swap(array_ctx *arr, int32_t pos1, int32_t pos2) {
    if (pos1 < 0) {
        pos1 += (int32_t)arr->size;
    }
    if (pos2 < 0) {
        pos2 += (int32_t)arr->size;
    }
    ASSERTAB(pos1 >= 0 && (uint32_t)pos1 < arr->size, "pos1 error.");
    ASSERTAB(pos2 >= 0 && (uint32_t)pos2 < arr->size, "pos2 error.");
    if (pos1 == pos2) {
        return;
    }
    char *a = (char *)arr->ptr + (size_t)pos1 * arr->elsize;
    char *b = (char *)arr->ptr + (size_t)pos2 * arr->elsize;
    memcpy(arr->tmp, a, arr->elsize);
    memcpy(a, b, arr->elsize);
    memcpy(b, arr->tmp, arr->elsize);
}
