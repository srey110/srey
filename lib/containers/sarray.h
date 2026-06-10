#ifndef SARRAY_H_
#define SARRAY_H_

#include "base/macro.h"

typedef struct array_ctx {
    uint32_t elsize;      // 单元素字节数（init 时指定）
    uint32_t size;        // 当前元素数量
    uint32_t maxsize;     // 当前分配容量
    void    *ptr;         // 数据存储数组
}array_ctx;
/// <summary>
/// 初始化数组
/// </summary>
/// <param name="arr">array_ctx</param>
/// <param name="elsize">单元素字节数，须 大于 0</param>
/// <param name="maxsize">期望初始容量，0 使用默认值 ARRAY_INIT_SIZE</param>
void array_init(array_ctx *arr, uint32_t elsize, uint32_t maxsize);
/// <summary>
/// 释放数组内部内存，不释放 arr 本身
/// </summary>
/// <param name="arr">array_ctx</param>
void array_free(array_ctx *arr);
/// <summary>
/// 调整数组容量（不缩减元素数量）
/// </summary>
/// <param name="arr">array_ctx</param>
/// <param name="maxsize">新容量，必须 大于等于 当前 size；0 使用默认值 ARRAY_INIT_SIZE</param>
void array_resize(array_ctx *arr, uint32_t maxsize);
/// <summary>
/// 在指定位置插入元素（pos 之后的元素整体后移）
/// </summary>
/// <param name="arr">array_ctx</param>
/// <param name="elem">指向待插入元素的指针，拷贝 elsize 字节</param>
/// <param name="pos">插入位置，[0, size]；负数表示从尾部反向索引</param>
void array_add(array_ctx *arr, const void *elem, int32_t pos);
/// <summary>
/// 删除指定位置元素（保持顺序，后续元素整体前移）
/// </summary>
/// <param name="arr">array_ctx</param>
/// <param name="pos">删除位置，[0, size)；负数表示从尾部反向索引</param>
void array_del(array_ctx *arr, int32_t pos);
/// <summary>
/// 交换两个位置的元素
/// </summary>
/// <param name="arr">array_ctx</param>
/// <param name="pos1">位置 1，[0, size)；负数表示从尾部反向索引</param>
/// <param name="pos2">位置 2，[0, size)；负数表示从尾部反向索引</param>
void array_swap(array_ctx *arr, int32_t pos1, int32_t pos2);
/// <summary>
/// 当前元素数量
/// </summary>
/// <param name="arr">array_ctx</param>
/// <returns>元素数量</returns>
static inline uint32_t array_size(array_ctx *arr) {
    return arr->size;
}
/// <summary>
/// 数组是否为空
/// </summary>
/// <param name="arr">array_ctx</param>
/// <returns>非 0 表示空，0 表示非空</returns>
static inline int32_t array_empty(array_ctx *arr) {
    return 0 == arr->size;
}
/// <summary>
/// 清空数组（保留已分配容量，下次复用）
/// </summary>
/// <param name="arr">array_ctx</param>
static inline void array_clear(array_ctx *arr) {
    arr->size = 0;
}
/// <summary>
/// 按索引访问元素（越界 ASSERT 终止）
/// </summary>
/// <param name="arr">array_ctx</param>
/// <param name="pos">索引；负数表示从尾部反向索引</param>
/// <returns>指向元素的指针（可隐式转 T *）</returns>
static inline void *array_at(array_ctx *arr, int32_t pos) {
    if (pos < 0) {
        pos += (int32_t)arr->size;
    }
    ASSERTAB(pos >= 0 && (uint32_t)pos < arr->size, "array pos error.");
    return (char *)arr->ptr + (size_t)pos * arr->elsize;
}
/// <summary>
/// 首元素指针
/// </summary>
/// <param name="arr">array_ctx</param>
/// <returns>首元素指针，空数组返回 NULL</returns>
static inline void *array_front(array_ctx *arr) {
    return 0 == arr->size ? NULL : arr->ptr;
}
/// <summary>
/// 末元素指针
/// </summary>
/// <param name="arr">array_ctx</param>
/// <returns>末元素指针，空数组返回 NULL</returns>
static inline void *array_back(array_ctx *arr) {
    return 0 == arr->size ? NULL : (char *)arr->ptr + (size_t)(arr->size - 1) * arr->elsize;
}
/// <summary>
/// 尾部追加元素（容量不足自动扩容到原 2 倍）
/// </summary>
/// <param name="arr">array_ctx</param>
/// <param name="elem">指向待追加元素的指针，拷贝 elsize 字节</param>
static inline void array_push_back(array_ctx *arr, const void *elem) {
    if (arr->size == arr->maxsize) {
        array_resize(arr, arr->maxsize * 2);
    }
    memcpy((char *)arr->ptr + (size_t)arr->size * arr->elsize, elem, arr->elsize);
    arr->size++;
}
/// <summary>
/// 弹出末元素
/// </summary>
/// <param name="arr">array_ctx</param>
/// <returns>指向已弹出元素的指针（下次 push 前有效），空数组返回 NULL</returns>
static inline void *array_pop_back(array_ctx *arr) {
    if (0 == arr->size) {
        return NULL;
    }
    arr->size--;
    return (char *)arr->ptr + (size_t)arr->size * arr->elsize;
}
/// <summary>
/// 删除指定位置元素（不保持顺序，用末元素填补，O(1)）
/// </summary>
/// <param name="arr">array_ctx</param>
/// <param name="pos">删除位置，[0, size)；负数表示从尾部反向索引</param>
static inline void array_del_nomove(array_ctx *arr, int32_t pos) {
    if (pos < 0) {
        pos += (int32_t)arr->size;
    }
    ASSERTAB(pos >= 0 && (uint32_t)pos < arr->size, "pos error.");
    arr->size--;
    if ((uint32_t)pos < arr->size) {
        memcpy((char *)arr->ptr + (size_t)pos * arr->elsize,
               (char *)arr->ptr + (size_t)arr->size * arr->elsize, arr->elsize);
    }
}

#endif//SARRAY_H_
