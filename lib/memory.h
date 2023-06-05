#ifndef MEMORY_H_
#define MEMORY_H_

#include "os.h"

typedef void *(*chr_func)(const void *, int32_t, size_t);
typedef int32_t(*cmp_func)(const void *, const void *, size_t);

void *_malloc(size_t size);
void *_calloc(size_t count, size_t size);
void *_realloc(void* oldptr, size_t size);
void _free(void* ptr);
void _memcheck(void);

#endif//MEMORY_H_
