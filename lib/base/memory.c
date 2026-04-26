#include "base/memory.h"
#include "base/macro.h"

#define _MALLOC  malloc
#define _CALLOC  calloc
#define _REALLOC realloc
#define _FREE    free

#if MEMORY_CHECK
    static atomic64_t _nalloc = 0; // 累计分配次数（原子计数）
    static atomic64_t _nfree  = 0; // 累计释放次数（原子计数）
#endif

void *_malloc(size_t size) {
#if MEMORY_CHECK
    ATOMIC64_ADD(&_nalloc, 1);
#endif
    void *ptr = _MALLOC(size);
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
    void *ptr = _CALLOC(count, size);
    if (NULL == ptr) {
        LOG_ERROR("calloc(%zu, %zu) failed!", count, size);
        exit(ERR_FAILED);
    }
    return ptr;
}
void *_realloc(void* oldptr, size_t size) {
#if MEMORY_CHECK
    /* realloc(NULL, n)  等价于 malloc(n)  — 只计 alloc，不计 free
     * realloc(p, n>0)   调整大小         — 净增减为 0，两者都不计
     * realloc(p, 0)     等价于 free(p)   — 只计 free，不计 alloc     */
    if (NULL == oldptr) {
        ATOMIC64_ADD(&_nalloc, 1);
    } else if (0 == size) {
        ATOMIC64_ADD(&_nfree, 1);
    }
    /* else: resize in-place, net accounting change = 0 */
#endif
    void *ptr = _REALLOC(oldptr, size);
    if (NULL == ptr && size > 0) {
        LOG_ERROR("realloc(%p, %zu) failed!", oldptr, size);
        exit(ERR_FAILED);
    }
    return ptr;
}
void _free(void* ptr) {
#if MEMORY_CHECK
    ATOMIC64_ADD(&_nfree, 1);
#endif
    _FREE(ptr);
}
void _memcheck(void) {
#if MEMORY_CHECK
    LOG_INFO("memory check => not free: %d.", (int32_t)(_nalloc - _nfree));
#endif
}
