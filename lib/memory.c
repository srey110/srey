#include "memory.h"
#include "macro_win.h"
#include "macro_unix.h"
#include "config.h"
#include "errcode.h"

#if MEMORY_CHECK
static volatile atomic64_t n_alloc = 0;
static volatile atomic64_t n_free = 0;
#endif

static inline uint64_t _nalloc()
{
#if MEMORY_CHECK
    return n_alloc;
#else
    return 0;
#endif
}
static inline uint64_t _nfree()
{
#if MEMORY_CHECK
    return n_free;
#else
    return 0;
#endif
}
void *_malloc(size_t size)
{
#if MEMORY_CHECK
    ATOMIC64_ADD(&n_alloc, 1);
#endif
    void *ptr = malloc(size);
    if (NULL == ptr)
    {
        fprintf(stderr, "malloc(%zu) failed!\n", size);
        exit(ERR_FAILED);
    }
    return ptr;
}
void *_calloc(size_t count, size_t size)
{
#if MEMORY_CHECK
    ATOMIC64_ADD(&n_alloc, 1);
#endif
    void *ptr = calloc(count, size);
    if (NULL == ptr)
    {
        fprintf(stderr, "calloc(%zu, %zu) failed!\n", count, size);
        exit(ERR_FAILED);
    }
    return ptr;
}
void *_realloc(void* oldptr, size_t size)
{
#if MEMORY_CHECK
    ATOMIC64_ADD(&n_alloc, 1);
    ATOMIC64_ADD(&n_free, 1);
#endif
    void *ptr = realloc(oldptr, size);
    if (NULL == ptr)
    {
        fprintf(stderr, "realloc(%p, %zu) failed!\n", oldptr, size);
        exit(ERR_FAILED);
    }
    return ptr;
}
void _free(void* ptr)
{
#if MEMORY_CHECK
    ATOMIC64_ADD(&n_free, 1);
#endif
    free(ptr);
}
void _memcheck(void)
{
#if MEMORY_CHECK
    printf("Memory check => alloc:%"PRIu64" free:%"PRIu64"\n", _nalloc(), _nfree());
#endif
}
