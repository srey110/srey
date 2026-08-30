#ifndef QUEUE_H_
#define QUEUE_H_

#include "base/macro.h"

typedef struct queue_ctx {
    uint32_t elsize;       // 单元素字节数（init 时指定）
    uint32_t offset;       // 队头偏移（循环起始位置）
    uint32_t size;         // 当前元素数量
    uint32_t maxsize;      // 当前分配容量
    void    *ptr;          // 数据存储数组
}queue_ctx;
/// <summary>
/// 初始化循环队列
/// </summary>
/// <param name="qu">queue_ctx</param>
/// <param name="elsize">单元素字节数，须 大于 0</param>
/// <param name="maxsize">期望初始容量，非 0 时向上取到偶数(同 queue_resize)；0 表示延迟分配
///   ——此刻不申请内存，首次入队(push 或 trypush 均可)时按默认容量分配。
///   给"多数实例可能一个元素都装不进来"的场景省掉预付内存</param>
void queue_init(queue_ctx *qu, uint32_t elsize, uint32_t maxsize);
/// <summary>
/// 释放队列内部内存，不释放 qu 本身
/// </summary>
/// <param name="qu">queue_ctx</param>
void queue_free(queue_ctx *qu);
/// <summary>
/// 调整队列容量；扩容后元素重排到新缓冲起始位置（offset 归 0）
/// </summary>
/// <param name="qu">queue_ctx</param>
/// <param name="maxsize">新容量，必须 大于等于 当前 size；0 使用默认值</param>
void queue_resize(queue_ctx *qu, uint32_t maxsize);
/// <summary>
/// 删除指定位置元素（保持顺序，后续元素整体前移）
/// </summary>
/// <param name="qu">queue_ctx</param>
/// <param name="pos">删除位置，[0, size)</param>
void queue_del_at(queue_ctx *qu, uint32_t pos);
// 环形下标回绕。前提是 off < 2 * maxsize —— offset 与 pos 各自都 < maxsize,
// 队列内部所有下标计算都满足它;不满足时这个减法回绕不到位, 会读到错误槽位
static inline uint32_t _queue_wrap(const queue_ctx *qu, uint32_t off) {
    return off >= qu->maxsize ? off - qu->maxsize : off;
}
/// <summary>
/// 当前元素数量
/// </summary>
/// <param name="qu">queue_ctx</param>
/// <returns>元素数量</returns>
static inline uint32_t queue_size(queue_ctx *qu) {
    return qu->size;
}
/// <summary>
/// 队列容量
/// </summary>
/// <param name="qu">queue_ctx</param>
/// <returns>当前分配容量</returns>
static inline uint32_t queue_maxsize(queue_ctx *qu) {
    return qu->maxsize;
}
/// <summary>
/// 队列是否为空
/// </summary>
/// <param name="qu">queue_ctx</param>
/// <returns>非 0 表示空，0 表示非空</returns>
static inline int32_t queue_empty(queue_ctx *qu) {
    return 0 == qu->size;
}
/// <summary>
/// 清空队列（保留已分配容量，下次复用）
/// </summary>
/// <param name="qu">queue_ctx</param>
static inline void queue_clear(queue_ctx *qu) {
    qu->size = 0;
    qu->offset = 0;
}
/// <summary>
/// 按队头偏移访问元素(不弹出)
/// </summary>
/// <param name="qu">queue_ctx</param>
/// <param name="pos">相对队头的偏移，[0, size)</param>
/// <returns>指向元素的指针(可隐式转 T *)，越界返 NULL</returns>
static inline void *queue_at(queue_ctx *qu, uint32_t pos) {
    if (pos >= qu->size) {
        return NULL;
    }
    uint32_t cur = _queue_wrap(qu, qu->offset + pos);
    return (char *)qu->ptr + (size_t)cur * qu->elsize;
}
/// <summary>
/// 查看队头元素（不弹出）
/// </summary>
/// <param name="qu">queue_ctx</param>
/// <returns>指向队头元素的指针，空队列返 NULL</returns>
static inline void *queue_peek(queue_ctx *qu) {
    return 0 == qu->size ? NULL : (char *)qu->ptr + (size_t)qu->offset * qu->elsize;
}
/// <summary>
/// 队尾追加元素（容量不足自动扩容到原 2 倍）
/// </summary>
/// <param name="qu">queue_ctx</param>
/// <param name="elem">指向待追加元素的指针，拷贝 elsize 字节</param>
static inline void queue_push(queue_ctx *qu, const void *elem) {
    if (qu->size == qu->maxsize) {
        ASSERTAB(qu->maxsize <= UINT32_MAX / 2, "queue maxsize overflow.");
        queue_resize(qu, qu->maxsize * 2);
    }
    uint32_t pos = _queue_wrap(qu, qu->offset + qu->size);
    memcpy((char *)qu->ptr + (size_t)pos * qu->elsize, elem, qu->elsize);
    qu->size++;
}
/// <summary>
/// 队列是否已满(元素数达当前分配容量)。queue_push 会自动扩容,只有想要"满就拒"的
/// 调用方才需要先问这个
/// </summary>
/// <param name="qu">queue_ctx</param>
/// <returns>非 0 表示已满。延迟分配态(maxsize 为 0)报未满——那是"还没申请"，不是"装不下"</returns>
static inline int32_t queue_full(queue_ctx *qu) {
    return 0 != qu->maxsize && qu->size >= qu->maxsize;
}
/// <summary>
/// 队尾追加元素,满则失败且不扩容。有界队列共用这一处判定,不必各写一遍
/// </summary>
/// <param name="qu">queue_ctx</param>
/// <param name="elem">指向待追加元素的指针，拷贝 elsize 字节</param>
/// <returns>ERR_OK 已入队；ERR_FAILED 队列已满,元素未写入</returns>
static inline int32_t queue_trypush(queue_ctx *qu, const void *elem) {
    if (queue_full(qu)) {
        return ERR_FAILED;
    }
    queue_push(qu, elem);
    return ERR_OK;
}
/// <summary>
/// 弹出队头元素
/// </summary>
/// <param name="qu">queue_ctx</param>
/// <returns>指向已弹出元素的指针(下次 push 前有效)，空队列返 NULL</returns>
static inline void *queue_pop(queue_ctx *qu) {
    if (0 == qu->size) {
        return NULL;
    }
    void *elem = (char *)qu->ptr + (size_t)qu->offset * qu->elsize;
    qu->offset = _queue_wrap(qu, qu->offset + 1);
    qu->size--;
    return elem;
}

#endif//QUEUE_H_
