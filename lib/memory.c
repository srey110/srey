#include "memory.h"
#include "macro.h"

#if MEMORY_CHECK
static atomic64_t _nalloc = 0;
static atomic64_t _nfree = 0;
#endif

void *_malloc(size_t size) {
#if MEMORY_CHECK
    ATOMIC64_ADD(&_nalloc, 1);
#endif
    void *ptr = malloc(size);
    if (NULL == ptr) {
        LOG_ERROR("malloc(%zu) failed!", size);
        exit(ERR_FAILED);
    }
    return ptr;
}
void *_calloc(size_t count, size_t size) {
#if MEMORY_CHECK
    ATOMIC64_ADD(&_nalloc, 1);
#endif
    void *ptr = calloc(count, size);
    if (NULL == ptr) {
        LOG_ERROR("calloc(%zu, %zu) failed!", count, size);
        exit(ERR_FAILED);
    }
    return ptr;
}
void *_realloc(void* oldptr, size_t size) {
#if MEMORY_CHECK
    ATOMIC64_ADD(&_nalloc, 1);
    ATOMIC64_ADD(&_nfree, 1);
#endif
    void *ptr = realloc(oldptr, size);
    if (NULL == ptr) {
        LOG_ERROR("realloc(%p, %zu) failed!", oldptr, size);
        exit(ERR_FAILED);
    }
    return ptr;
}
void _free(void* ptr) {
#if MEMORY_CHECK
    ATOMIC64_ADD(&_nfree, 1);
#endif
    free(ptr);
}
void _memcheck(void) {
#if MEMORY_CHECK
    LOG_INFO("memory check => not free: %d.", (int32_t)(_nalloc - _nfree));
#endif
}
