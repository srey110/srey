#include "base/memory.h"
#include "base/macro.h"

#define _MALLOC  malloc
#define _CALLOC  calloc
#define _REALLOC realloc
#define _FREE    free

#if MEMORY_CHECK
/* 分条计数：两个相邻的全局计数器会让每次 malloc/free 都在同一条 cache line 上跨核来回。
 * 改成每线程独占一格、各占一条 cache line，槽位用尽(活过的线程数超过 MEM_SLOTS)的
 * 线程共用末尾那一格 —— 两条路径的计数都精确。*/
#define MEM_SLOTS 64 // 独占槽位数;只增不回收,用尽即共用末尾那格
typedef struct mem_slot {
    atomic64_t nalloc; // 本槽位累计分配次数
    atomic64_t nfree;  // 本槽位累计释放次数
    char pad[CACHELINE_SIZE - 2 * sizeof(atomic64_t)];
}mem_slot;
CACHELINE_ALIGN static mem_slot _slots[MEM_SLOTS + 1];// 末一格给槽位用尽的线程共用
static atomic64_t _slotseq = 0; // 槽位分配游标
static THREAD_LOCAL mem_slot *_slot = NULL;
#endif

#if MEMORY_CHECK && MEMORY_TRACE && !defined(OS_AIX)
#define MEM_TRK_BUCKET 65536 // 活动分配哈希桶数（2 的幂）
#define MEM_TRK_FRAMES 32 // 单条记录最大栈帧数
#if defined(OS_WIN)
    static SRWLOCK _trk_lock = SRWLOCK_INIT;
    #define MEM_TRK_LOCK()   AcquireSRWLockExclusive(&_trk_lock)
    #define MEM_TRK_UNLOCK() ReleaseSRWLockExclusive(&_trk_lock)
#else
    static pthread_mutex_t _trk_lock = PTHREAD_MUTEX_INITIALIZER;
    #define MEM_TRK_LOCK()   pthread_mutex_lock(&_trk_lock)
    #define MEM_TRK_UNLOCK() pthread_mutex_unlock(&_trk_lock)
#endif
// 活动分配记录：ptr -> 调用栈，按 ptr 哈希链式存储
typedef struct mem_trk_ctx {
    int32_t frames;
    void *ptr;
    struct mem_trk_ctx *next;
    void *stack[MEM_TRK_FRAMES];
}mem_trk_ctx;
static mem_trk_ctx *_trk_bucket[MEM_TRK_BUCKET];
// ptr 哈希到桶下标（低位通常为对齐 0，右移消除）
static size_t _trk_hash(void *ptr) {
    return ((uintptr_t)ptr >> 4) & (MEM_TRK_BUCKET - 1);
}
// 捕获当前调用栈，跳过本函数与 _malloc 等包装帧
static int32_t _trk_capture(void **stack, int32_t max) {
#if defined(OS_WIN)
    return (int32_t)CaptureStackBackTrace(2, (DWORD)max, stack, NULL);
#else
    void *tmp[MEM_TRK_FRAMES + 2];
    int32_t n = backtrace(tmp, max + 2);
    n = n > 2 ? n - 2 : 0;
    memcpy(stack, tmp + 2, (size_t)n * sizeof(void *));
    return n;
#endif
}
// 记录一次分配（节点用系统 malloc，不计入 _nalloc/_nfree）
static void _trk_add(void *ptr) {
    if (NULL == ptr) {
        return;
    }
    mem_trk_ctx *node = (mem_trk_ctx *)_MALLOC(sizeof(mem_trk_ctx));
    if (NULL == node) {
        return;
    }
    node->ptr = ptr;
    node->frames = _trk_capture(node->stack, MEM_TRK_FRAMES);
    MEM_TRK_LOCK();
    size_t slot = _trk_hash(ptr);
    node->next = _trk_bucket[slot];
    _trk_bucket[slot] = node;
    MEM_TRK_UNLOCK();
}
// 移除一次分配记录
static void _trk_del(void *ptr) {
    if (NULL == ptr) {
        return;
    }
    MEM_TRK_LOCK();
    mem_trk_ctx *dead;
    mem_trk_ctx **pp = &_trk_bucket[_trk_hash(ptr)];
    while (NULL != *pp) {
        if ((*pp)->ptr == ptr) {
            dead = *pp;
            *pp = dead->next;
            _FREE(dead);
            break;
        }
        pp = &(*pp)->next;
    }
    MEM_TRK_UNLOCK();
}
// 符号化打印一条未释放块的调用栈
static void _trk_print(const mem_trk_ctx *node) {
    fprintf(stderr, "[memory leak] %p:\n", node->ptr);
#if defined(OS_WIN)
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    HANDLE proc = GetCurrentProcess();
    DWORD64 disp;
    for (int32_t i = 0; i < node->frames; i++) {
        disp = 0;
        if (SymFromAddr(proc, (DWORD64)(uintptr_t)node->stack[i], &disp, sym)) {
            fprintf(stderr, "  %2d %s + 0x%llx\n", i, sym->Name, (unsigned long long)disp);
        } else {
            fprintf(stderr, "  %2d %p\n", i, node->stack[i]);
        }
    }
#else
    backtrace_symbols_fd(node->stack, node->frames, 2);
#endif
}
// 遍历所有桶，dump 仍存活（未释放）的分配
static void _trk_dump(void) {
    MEM_TRK_LOCK();
#if defined(OS_WIN)
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
#endif
    mem_trk_ctx *node;
    for (size_t slot = 0; slot < MEM_TRK_BUCKET; slot++) {
        for (node = _trk_bucket[slot]; NULL != node; node = node->next) {
            _trk_print(node);
        }
    }
#if defined(OS_WIN)
    SymCleanup(GetCurrentProcess());
#endif
    MEM_TRK_UNLOCK();
}
#endif//MEMORY_CHECK && MEMORY_TRACE

#if MEMORY_CHECK
// 首次调用给本线程钉一格,此后只自增。末尾那格可能被多个线程共用,原子自增照样精确
static void _mem_count(int32_t is_alloc) {
    if (NULL == _slot) {
        int64_t seq = ATOMIC64_ADD(&_slotseq, 1);// 返回旧值
        _slot = &_slots[(seq < MEM_SLOTS) ? (size_t)seq : MEM_SLOTS];
    }
    ATOMIC64_ADD_RELAXED(is_alloc ? &_slot->nalloc : &_slot->nfree, 1);
}
#endif//MEMORY_CHECK
void mem_stat(uint64_t *nalloc, uint64_t *nfree) {
#if MEMORY_CHECK
    uint64_t na = 0;
    uint64_t nf = 0;
    int32_t i;
    for (i = 0; i <= MEM_SLOTS; i++) {
        na += (uint64_t)ATOMIC64_GET(&_slots[i].nalloc);
        nf += (uint64_t)ATOMIC64_GET(&_slots[i].nfree);
    }
    SET_PTR(nalloc, na);
    SET_PTR(nfree, nf);
#else
    SET_PTR(nalloc, 0);
    SET_PTR(nfree, 0);
#endif
}

void *_malloc(size_t size) {
#if MEMORY_CHECK
    _mem_count(1);
#endif
    void *ptr = _MALLOC(size);
    if (NULL == ptr) {
        LOG_ERROR("malloc(%zu) failed!", size);
        exit(ERR_FAILED);
    }
#if MEMORY_CHECK && MEMORY_TRACE && !defined(OS_AIX)
    _trk_add(ptr);
#endif
    return ptr;
}
void *_calloc(size_t count, size_t size) {
#if MEMORY_CHECK
    _mem_count(1);
#endif
    void *ptr = _CALLOC(count, size);
    if (NULL == ptr) {
        LOG_ERROR("calloc(%zu, %zu) failed!", count, size);
        exit(ERR_FAILED);
    }
#if MEMORY_CHECK && MEMORY_TRACE && !defined(OS_AIX)
    _trk_add(ptr);
#endif
    return ptr;
}
void *_realloc(void* oldptr, size_t size) {
#if MEMORY_CHECK
    if (NULL == oldptr && 0 != size) {
        _mem_count(1);
    } else if (NULL != oldptr && 0 == size) {
        _mem_count(0);
    }
    // (NULL, 0) no-op + (非NULL, >0) realloc 改大小，均不计数
#endif
    if (0 == size) {
#if MEMORY_CHECK && MEMORY_TRACE && !defined(OS_AIX)
        _trk_del(oldptr);
#endif
        _FREE(oldptr);
        return NULL;
    }
#if MEMORY_CHECK && MEMORY_TRACE && !defined(OS_AIX)
    if (NULL != oldptr) {
        _trk_del(oldptr);
    }
#endif
    void *ptr = _REALLOC(oldptr, size);
    if (NULL == ptr) {
        LOG_ERROR("realloc(%p, %zu) failed!", oldptr, size);
        exit(ERR_FAILED);
    }
#if MEMORY_CHECK && MEMORY_TRACE && !defined(OS_AIX)
    _trk_add(ptr);
#endif
    return ptr;
}
void _free(void* ptr) {
    if (NULL == ptr) {
        return;
    }
#if MEMORY_CHECK
    _mem_count(0);
#endif
#if MEMORY_CHECK && MEMORY_TRACE && !defined(OS_AIX)
    _trk_del(ptr);
#endif
    _FREE(ptr);
}
void _memcheck(void) {
#if MEMORY_CHECK
    uint64_t na, nf;
    mem_stat(&na, &nf);
    int64_t leak = (int64_t)na - (int64_t)nf;
    PRINT("memory check => not free: %" PRId64 ".", leak);
#if MEMORY_TRACE && !defined(OS_AIX)
    if (0 != leak) {
        _trk_dump();
    }
#endif
#endif
}
