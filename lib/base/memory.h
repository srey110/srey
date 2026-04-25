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
void _memcheck(void);

#endif//MEMORY_H_
