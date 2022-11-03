#ifndef MEMORY_H_
#define MEMORY_H_

#include "macro_win.h"
#include "macro_unix.h"
#include "errcode.h"

#define PRINT_DEBUG

void *_malloc(size_t size);
void *_calloc(size_t count, size_t size);
void *_realloc(void* oldptr, size_t size);
void _free(void* ptr);
void _memcheck(void);

#endif//MEMORY_H_
