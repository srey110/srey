#include "base/memory.h"
#include "base/err.h"
#include "base/config.h"
#include "base/macro_util.h"
#include "base/macro_atomic.h"
#include "base/macro_log.h"

// 全项目的分配都收在这四个宏,换分配器只动这里。头与库由 tools/deps.py 摆到 lib/ 与 bin/
#if WITH_MIMALLOC
    #if ENABLED_ASAN || ENABLED_TSAN
        #error "WITH_MIMALLOC conflicts with ASan/TSan: both hook the allocator. Turn it off in config.h"
    #endif
    #include "mimalloc/mimalloc.h"
    #define _MALLOC  mi_malloc
    #define _CALLOC  mi_calloc
    #define _REALLOC mi_realloc
    #define _FREE    mi_free
#else
    #define _MALLOC  malloc
    #define _CALLOC  calloc
    #define _REALLOC realloc
    #define _FREE    free
#endif//WITH_MIMALLOC

#define MEM_ARENA_BLOCK 8192 // mem_arena 每个定长块的字节数
#define MEM_ARENA_HEAD ROUND_UP(sizeof(mem_arena_blk), 8) // 块头按 8 对齐后的字节数
#if !defined(CC_GNU)
typedef void *(*memset_func)(void *, int, size_t);// secure_zero 在 MSVC 下经 volatile 指针调 memset 用
// MSVC 没有空汇编屏障，secure_zero 经它调 memset。指针是 volatile：每次调用都得现读，编译器断定不了它指向 memset，
// 就不会把清零当成"之后没人读的写"删掉
static volatile memset_func _memset_vol = memset;
#endif//CC_GNU
// 分配追踪要同时满足三个条件,下面所有相关段落统一判这一个
#if MEMORY_CHECK && MEMORY_TRACE && defined(HAVE_BACKTRACE)
    #define MEM_TRACE_ON 1
#else
    #define MEM_TRACE_ON 0
#endif//MEMORY_CHECK MEMORY_TRACE HAVE_BACKTRACE

#if MEMORY_CHECK
/* 分条计数：每线程分到一格、各占一条 cache line，免得每次 malloc/free 都在同一条
 * cache line 上跨核来回。槽位用尽(活过的线程数超过 MEM_SLOTS)的线程共用末尾那一格。
 * 每格都原子加(理由见 _mem_count)，计数精确。*/
// 独占槽位数;只增不回收,用尽即共用末尾那格(那一格上每次分配都要跨核争抢,慢一个数量级以上)。
// 线程数约 nnet + nworker + 4,两者取 0 时按核数算,取 256 够 120 核上下的机器
#define MEM_SLOTS 256
typedef struct mem_slot {
    atomic64_t nalloc; // 本槽位累计分配次数
    atomic64_t nfree;  // 本槽位累计释放次数
    char pad[CACHELINE_SIZE - 2 * sizeof(atomic64_t)];
}mem_slot;
CACHELINE_ALIGN static mem_slot _slots[MEM_SLOTS + 1];// 末一格给槽位用尽的线程共用
static atomic64_t _slotseq = 0; // 槽位分配游标
static THREAD_LOCAL mem_slot *_slot = NULL;// TLS_RAW_OK：计数一律原子加，协程换线程后读到别的线程的格也不丢计数
#endif//MEMORY_CHECK

#if MEM_TRACE_ON
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
#endif//OS_WIN
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
    mem_trk_ctx *dead = NULL;
    MEM_TRK_LOCK();
    mem_trk_ctx **pp = &_trk_bucket[_trk_hash(ptr)];
    while (NULL != *pp) {
        if ((*pp)->ptr == ptr) {
            dead = *pp;
            *pp = dead->next;
            break;
        }
        pp = &(*pp)->next;
    }
    MEM_TRK_UNLOCK();
    _FREE(dead);
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
#endif//MEM_TRACE_ON

#if MEMORY_CHECK
// 首次调用给本线程钉一格,此后只自增。格只管分流,不保证只有本线程写:协程在别的线程上恢复后,
// 编译器复用挂起前的 TLS 地址,会加到原线程那格上。所以一律原子加,读写两步会跟原线程互相吃掉计数
static inline void _mem_count(int32_t is_alloc) {
    if (NULL == _slot) {
        int64_t seq = ATOMIC64_ADD_RELAXED(&_slotseq, 1);// 返回旧值
        _slot = &_slots[(seq < MEM_SLOTS) ? (size_t)seq : MEM_SLOTS];
    }
    ATOMIC64_ADD_RELAXED(is_alloc ? &_slot->nalloc : &_slot->nfree, 1);
}
#endif//MEMORY_CHECK
void mem_stat(uint64_t *nalloc, uint64_t *nfree) {
#if MEMORY_CHECK
    uint64_t na = (uint64_t)ATOMIC64_GET_RELAXED(&_slots[MEM_SLOTS].nalloc);
    uint64_t nf = (uint64_t)ATOMIC64_GET_RELAXED(&_slots[MEM_SLOTS].nfree);
    uint64_t n = (uint64_t)ATOMIC64_GET_RELAXED(&_slotseq);
    uint64_t i;
    if (n > MEM_SLOTS) {
        n = MEM_SLOTS;
    }
    for (i = 0; i < n; i++) {
        na += (uint64_t)ATOMIC64_GET_RELAXED(&_slots[i].nalloc);
        nf += (uint64_t)ATOMIC64_GET_RELAXED(&_slots[i].nfree);
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
#if MEM_TRACE_ON
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
#if MEM_TRACE_ON
    _trk_add(ptr);
#endif
    return ptr;
}
void *_realloc(void* oldptr, size_t size) {
    if (NULL == oldptr) {
        return 0 == size ? NULL : _malloc(size);
    }
    if (0 == size) {
        _free(oldptr);
        return NULL;
    }
#if MEM_TRACE_ON
    _trk_del(oldptr);
#endif
    void *ptr = _REALLOC(oldptr, size);
    if (NULL == ptr) {
        LOG_ERROR("realloc(%p, %zu) failed!", oldptr, size);
        exit(ERR_FAILED);
    }
#if MEM_TRACE_ON
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
#if MEM_TRACE_ON
    _trk_del(ptr);
#endif
    _FREE(ptr);
}
int64_t _memcheck(void) {
#if MEMORY_CHECK
    uint64_t na, nf;
    mem_stat(&na, &nf);
    int64_t leak = (int64_t)na - (int64_t)nf;
    PRINT("memory check => not free: %" PRId64 ".", leak);
#if MEM_TRACE_ON
    if (0 != leak) {
        _trk_dump();
    }
#endif
    return leak;
#else
    return 0;
#endif
}
void *_mem_arena_alloc_slow(mem_arena *arena, size_t lens) {
    mem_arena_blk *blk;
    // 一整块都装不下：按实际大小单开一块，整块只给这一次分配用
    if (lens > MEM_ARENA_BLOCK - MEM_ARENA_HEAD) {
        MALLOC(blk, MEM_ARENA_HEAD + lens);
        if (NULL == arena->cur) {
            // 空链：它就是当前块，off 与 cap 相等表示已用满，下次分配会再开新块
            blk->next = NULL;
            arena->cur = blk;
            arena->off = arena->cap = MEM_ARENA_HEAD + lens;
        } else {
            // 已有当前块：插到它后面而不是链头，当前块没用完的空间后面还能接着切
            blk->next = arena->cur->next;
            arena->cur->next = blk;
        }
        return (char *)blk + MEM_ARENA_HEAD;
    }
    // 当前块剩余不够：开一个定长新块插在链头当作新的当前块，旧块剩下的空间不再用
    MALLOC(blk, MEM_ARENA_BLOCK);
    blk->next = arena->cur;
    arena->cur = blk;
    arena->off = MEM_ARENA_HEAD + lens;
    arena->cap = MEM_ARENA_BLOCK;
    return (char *)blk + MEM_ARENA_HEAD;
}
void mem_arena_free(mem_arena *arena) {
    mem_arena_blk *blk;
    while (NULL != arena->cur) {
        blk = arena->cur;
        arena->cur = blk->next;
        FREE(blk);
    }
    arena->off = 0;
    arena->cap = 0;
}
// 两支各有一道承重的防删手段，去掉任何一道，-O2 -flto 内联进调用方后清零会被当成无用的写整段删掉。
// GCC/Clang 用空汇编屏障，memset 仍能内联成几条向量写(摘要每个块都擦一次调度表，别换成函数指针)；MSVC 走 _memset_vol
void secure_zero(void *buf, size_t len) {
    if (EMPTYPTR(buf, len)) {
        return;
    }
#if defined(CC_GNU)
    memset(buf, 0, len);
    // 空汇编：声明它读了 buf、还可能读写任意内存，编译器只好把上面的 memset 真正写出去
    __asm__ __volatile__("" : : "r"(buf) : "memory");
#else
    _memset_vol(buf, 0, len);
#endif
}
