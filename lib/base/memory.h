#ifndef MEMORY_H_
#define MEMORY_H_

#include "base/os.h"
#include "base/macro_util.h"

#define MALLOC(ptr, size) *(void**)&(ptr) = _malloc(size) // 分配内存并赋值给指针
#define CALLOC(ptr, count, size) *(void**)&(ptr) = _calloc(count, size) // 分配并清零内存
#define REALLOC(ptr, oldptr, size) *(void**)&(ptr) = _realloc(oldptr, size) // 重新分配内存
// 释放内存并将指针置为 NULL，避免悬空指针
#define FREE(ptr)\
    do {\
        if (NULL != ptr) {\
            _free(ptr); \
            ptr = NULL; \
        }\
    } while(0)
#define SECURE_FREE(ptr, lens)\
    do {\
        secure_zero(ptr, lens);\
        FREE(ptr);\
    } while(0)
// copy=0（本层已接管 data 所有权）时释放 data；copy!=0（复制语义）不释放。ev_send/task_* 接管失败或无接管的兜底
#define CHECK_COPY_FREE(data, copy)\
    do {\
        if (!(copy)) {\
            FREE(data);\
        }\
    } while(0)

// 块链：从定长块里顺序切，只往当前块(链头)追加，放不下就开新块、
// 旧块剩余作废；比一整块还大的单独开一块挂在当前块后面，当前块剩余照用。
// 切出的内存不能单独释放，清零即空链。
// 使用方：mysql / pgsql 结果集行、mysql 列定义包、redis 回复节点
// 块链里每块开头的块头，后面紧跟可切的区域
typedef struct mem_arena_blk {
    struct mem_arena_blk *next;// 链上的下一块，最后一块为 NULL
}mem_arena_blk;
typedef struct mem_arena {
    mem_arena_blk *cur;// 当前正在切的块；新块插在链头，所以它也是整条链的起点
    size_t off;// 当前块已用字节数(含块头)
    size_t cap;// 当前块总字节数
}mem_arena;

/// <summary>
/// 分配内存，失败时打印日志并终止程序
/// </summary>
/// <param name="size">分配字节数</param>
/// <returns>分配到的内存指针，永不返回 NULL</returns>
void *_malloc(size_t size);
/// <summary>
/// 分配并清零内存，失败时打印日志并终止程序
/// </summary>
/// <param name="count">元素个数</param>
/// <param name="size">单个元素字节数</param>
/// <returns>分配到的内存指针，永不返回 NULL</returns>
void *_calloc(size_t count, size_t size);
/// <summary>
/// 重新分配内存，失败时打印日志并终止程序
/// </summary>
/// <param name="oldptr">原内存指针，NULL 等同于 malloc</param>
/// <param name="size">新大小字节数，0 等同于 free</param>
/// <returns>新内存指针，size 为 0 时返回 NULL</returns>
void *_realloc(void* oldptr, size_t size);
/// <summary>
/// 释放内存
/// </summary>
/// <param name="ptr">要释放的内存指针</param>
void _free(void* ptr);
/// <summary>
/// 打印内存分配/释放统计信息（仅 MEMORY_CHECK 启用时有效）
/// </summary>
/// <returns>存活块数 = 累计分配 - 累计释放。0 为收支平衡，负数说明释放多于分配。
/// MEMORY_CHECK 关闭时恒为 0，此时检测不到泄漏，调用方不可据此判定通过</returns>
int64_t _memcheck(void);
/// <summary>
/// 汇总所有分条槽位，读取累计内存分配/释放次数。运行期拿到的是近似值：别的线程还在自增，
/// 两个出参也不是同一时刻的快照，nfree 可能读得比 nalloc 大——要算存活数得先比大小再相减。
/// _memcheck 在全线程 join 之后调用，那时精确
/// </summary>
/// <param name="nalloc">出参：累计分配次数，MEMORY_CHECK 关闭时写 0；可为 NULL 表示不关心</param>
/// <param name="nfree">出参：累计释放次数，MEMORY_CHECK 关闭时写 0；可为 NULL 表示不关心</param>
void mem_stat(uint64_t *nalloc, uint64_t *nfree);
// mem_arena_alloc 当前块放不下时的慢路径(开新块 / 单开大块)，lens 已按 8 取整，只由它调用
void *_mem_arena_alloc_slow(mem_arena *arena, size_t lens);
/// <summary>
/// 从块链里切一段内存，按 8 字节对齐。使用方见 mem_arena
/// </summary>
/// <param name="arena">块链；清零的结构即空链</param>
/// <param name="lens">字节数</param>
/// <returns>内存起点，随 mem_arena_free 一起释放，不能单独释放</returns>
static inline void *mem_arena_alloc(mem_arena *arena, size_t lens) {
    char *blk;
    lens = ROUND_UP(lens, 8);
    if (NULL != arena->cur
        && lens <= arena->cap - arena->off) {
        blk = (char *)arena->cur + arena->off;
        arena->off += lens;
        return blk;
    }
    return _mem_arena_alloc_slow(arena, lens);
}
/// <summary>
/// 释放块链里的全部块，之后 arena 回到空链，可以接着用
/// </summary>
/// <param name="arena">块链</param>
void mem_arena_free(mem_arena *arena);
/// <summary>
/// 安全清零缓冲区。与 ZERO/memset 不同，保证写入不被编译器优化掉（含 LTO），
/// 适用于密钥、密码、PBKDF2 中间值等使用后须立即抹除的敏感缓冲。
/// </summary>
/// <param name="buf">目标缓冲区（NULL 时直接返回）</param>
/// <param name="len">字节数（0 时直接返回）</param>
void secure_zero(void *buf, size_t len);

#endif//MEMORY_H_
