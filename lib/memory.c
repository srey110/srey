#include "memory.h"

#ifdef PRINT_DEBUG
static atomic64_t n_alloc = 0;
static atomic64_t n_free = 0;
#endif

static uint64_t _nalloc()
{
#ifdef PRINT_DEBUG
    return n_alloc;
#else
    return 0;
#endif    
}
static uint64_t _nfree()
{
#ifdef PRINT_DEBUG
    return n_free;
#else
    return 0;
#endif
}
void *_malloc(size_t size)
{
#ifdef PRINT_DEBUG
    ATOMIC64_ADD(&n_alloc, 1);
#endif
    void *ptr = malloc(size);
    if (NULL == ptr)
    {
        fprintf(stderr, "%s", "malloc failed!\n");
        exit(ERR_FAILED);
    }
    return ptr;
}
void *_calloc(size_t count, size_t size)
{
#ifdef PRINT_DEBUG
    ATOMIC64_ADD(&n_alloc, 1);
#endif
    void *ptr = calloc(count, size);
    if (NULL == ptr)
    {
        fprintf(stderr, "%s", "calloc failed!\n");
        exit(ERR_FAILED);
    }
    return ptr;
}
void *_realloc(void* oldptr, size_t size)
{
#ifdef PRINT_DEBUG
    ATOMIC64_ADD(&n_alloc, 1);
    ATOMIC64_ADD(&n_free, 1);
#endif
    void *ptr = realloc(oldptr, size);
    if (NULL == ptr)
    {
        fprintf(stderr, "%s", "realloc failed!\n");
        exit(ERR_FAILED);
    }
    return ptr;
}
void _free(void* ptr)
{
#ifdef PRINT_DEBUG
    ATOMIC64_ADD(&n_free, 1);
#endif
    free(ptr);
}
void memcheck(void)
{
#ifdef PRINT_DEBUG
    printf("Memory check => alloc:%"PRIu64" free:%"PRIu64"\n", _nalloc(), _nfree());
#endif
}
