#ifndef HASHSET_H_
#define HASHSET_H_

#include "base/macro.h"

typedef struct hashset hashset;

/// <summary>
/// 创建 hashset。元素 by-value 存储,内部基于项目分配函数(_malloc/_realloc/_free)。
/// 与 hashmap 区别:只存元素无 value 字段,只提供 add/contains/remove 集合语义。
/// </summary>
/// <param name="elsize">元素大小(bytes)</param>
/// <param name="cap">初始容量;0 用默认</param>
/// <param name="hash">hash 函数;签名与 hashmap 兼容,可直接传 hashmap_sip/hashmap_xxhash3,
///     用户自写时可忽略 seed0/seed1(内部固定 0)</param>
/// <param name="compare">比较函数;返回 0 相等</param>
/// <param name="elfree">元素释放回调(by-value 元素含指针时用,可 NULL)</param>
/// <param name="udata">透传给 compare 的上下文(可 NULL)</param>
/// <returns>hashset 指针;失败返 NULL</returns>
hashset *hashset_new(size_t elsize, size_t cap,
                     uint64_t (*hash)(const void *item, uint64_t seed0, uint64_t seed1),
                     int (*compare)(const void *a, const void *b, void *udata),
                     void (*elfree)(void *item),
                     void *udata);
/// <summary>释放 hashset。若设置了 elfree,会对所有元素调用一次</summary>
/// <param name="s">hashset 指针;NULL 安全</param>
void hashset_free(hashset *s);
/// <summary>清空所有元素;update_cap=非 0 同时缩回初始容量</summary>
/// <param name="s">hashset 指针</param>
/// <param name="update_cap">0 仅清条目;非 0 同时缩容</param>
void hashset_clear(hashset *s, int32_t update_cap);
/// <summary>当前元素数</summary>
/// <param name="s">hashset 指针</param>
/// <returns>元素个数</returns>
size_t hashset_count(const hashset *s);
/// <summary>最近一次 add 因内存分配失败时为 OOM 状态</summary>
/// <param name="s">hashset 指针</param>
/// <returns>1 OOM(本次 add 未成功);0 正常</returns>
int32_t hashset_oom(const hashset *s);
/// <summary>
/// 加入元素。已存在时不更新;新增时按 elsize 字节 by-value 拷贝。
/// </summary>
/// <param name="s">hashset 指针</param>
/// <param name="item">元素指针;按 elsize 字节读取</param>
/// <returns>1 新增;0 已存在;-1 内存分配失败(可用 hashset_oom 二次确认)</returns>
int32_t hashset_add(hashset *s, const void *item);
/// <summary>判断元素是否存在</summary>
/// <param name="s">hashset 指针</param>
/// <param name="item">查询元素</param>
/// <returns>1 存在;0 不存在</returns>
int32_t hashset_contains(const hashset *s, const void *item);
/// <summary>
/// 删除元素。若设置了 elfree,删除后不自动调用,由调用方按需处理。
/// </summary>
/// <param name="s">hashset 指针</param>
/// <param name="item">待删除元素</param>
/// <returns>hashmap 内部 item 指针(指向被删除元素副本,下次 add/grow 前有效);
///     元素不存在返 NULL</returns>
const void *hashset_remove(hashset *s, const void *item);
/// <summary>
/// 遍历全部元素。iter 返回 0 终止遍历;遍历期间不可增删,否则未定义行为。
/// </summary>
/// <param name="s">hashset 指针</param>
/// <param name="iter">每个元素调用一次;返回非 0 继续,返 0 终止</param>
/// <param name="udata">透传给 iter 的上下文</param>
/// <returns>1 遍历完;0 被 iter 提前终止</returns>
int32_t hashset_scan(hashset *s, int32_t (*iter)(const void *item, void *udata), void *udata);
/// <summary>迭代器风格遍历;遍历期间不可增删</summary>
/// <param name="s">hashset 指针</param>
/// <param name="i">迭代器状态;首次传入 *i=0,每次调用后内部自增</param>
/// <param name="item">出参:指向当前元素</param>
/// <returns>1 取到元素;0 遍历结束</returns>
int32_t hashset_iter(hashset *s, size_t *i, void **item);

#endif//HASHSET_H_
