#ifndef MEMORY_H_
#define MEMORY_H_

#include "base/os.h"

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

#endif//MEMORY_H_
