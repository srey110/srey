#ifndef ROUTER_TREE_H_
#define ROUTER_TREE_H_

#include "base/base.h"
#include "advance/router_pub.h"

// 段级路由树：静态段用 radix 索引，参数段/末尾通配各一条边，叶子存方法条目。
// 树只认 router_seg / buf_ctx / router_method，不认 handler、task 和 HTTP pack。
//
// 可选段 {name?} 在注册期展开成具体形状。
// 同一具体形状的多个方法条目按注册顺序排队，匹配取第一个覆盖当前方法的条目。
// 树注册完只读；rtree_match 不分配内存，通配值写调用方 scratch。

typedef struct rtree_ctx rtree_ctx;

// 一个可选段组合展开后的变体。
// opt_bits 的 bit i 对应“第 i 个 OPT 段是否出现”，不是原始 segs 下标。
// param_seg 按参数出现顺序记录原始 segs 下标，末尾通配也占一项。
typedef struct rtree_variant {
    uint32_t opt_bits;      // bit i = 第 i 个 OPT 段是否出现
    uint16_t param_seg[ROUTER_MAX_PARAMS]; // 参数按出现顺序 -> 原始 segs 下标
    uint8_t param_n;        // 参数个数，含末尾通配
} rtree_variant;
// 匹配得到的参数值；key 由 router 层用 variant->param_seg 回填。
typedef struct rtree_param {
    const uint8_t *val; // 指向 segs 或 scratch，不复制
    uint32_t len;       // 参数值字节数
} rtree_param;
typedef struct rtree_match_result {
    router_route_id route_id;   // 未命中为 ROUTER_ROUTE_NONE
    uint32_t variant_id;        // 命中变体在 rtree_add_result.variants 中的下标
    router_method allow;        // -4 / -5 时有效
    int32_t params_n;           // 参数值个数
    int32_t path_hit;           // 内部诊断用：是否有路径但没有方法命中
    rtree_param params[ROUTER_MAX_PARAMS]; // 只填值，key 由 router 层回填
} rtree_match_result;
// 注册结果。variants 由树分配，调用方用 rtree_add_result_free 释放。
// variants 包含所有规范化变体（含被遮住的），leaf 里的 variant_id 可直接索引它。
typedef struct rtree_add_result {
    rtree_variant *variants; // 树分配，rtree_add_result_free 释放
    int32_t n;          // 规范化变体数
    int32_t inserted;   // 真正插入叶子的变体数
    int32_t shadowed;   // 被已有叶子完全遮住的变体数
    int32_t skipped;    // 参数超限跳过的变体数
} rtree_add_result;

/// <summary>
/// 创建空树，根节点 id 为 0。
/// </summary>
/// <returns>树指针；内存分配失败由 MALLOC 终止进程，不会返回 NULL</returns>
rtree_ctx *rtree_new(void);
/// <summary>
/// 释放树及全部节点、叶子、字面量 radix；不释放 route handler、route segs 字符串。
/// </summary>
/// <param name="t">树</param>
void rtree_free(rtree_ctx *t);
/// <summary>
/// 注册一条路由：生成可选段变体并插入。部分变体被遮住仍算成功。
/// </summary>
/// <param name="t">树</param>
/// <param name="segs">已解析的段序列；WILD 必须是最后一段，OPT 只能在末尾（其后只许 OPT 或 WILD）</param>
/// <param name="n">段数</param>
/// <param name="mask">方法位掩码，不能为 0</param>
/// <param name="route_id">调用方分配的路由 id，树只保存不释放</param>
/// <param name="res">变体/遮蔽/跳过统计；variants 由调用方释放</param>
/// <returns>ERR_OK=至少一个变体入树；-1=res 为 NULL、t / segs 非法、段数超上限、mask 为 0、route_id 越界、
/// WILD 不在末段、OPT 不在末尾或超 ROUTER_MAX_OPT、全部变体参数超限；
/// -2=全部变体被已有路由完全遮住。失败时 res 里仍可能有 variants，调用方照常释放</returns>
int32_t rtree_add(rtree_ctx *t, const router_seg *segs, int32_t n, router_method mask, router_route_id route_id,
                  rtree_add_result *res);
/// <summary>
/// 在已解析段上匹配；不分配内存，通配值写 scratch。
/// </summary>
/// <param name="t">树</param>
/// <param name="segs">已解码路径段</param>
/// <param name="nseg">段数</param>
/// <param name="scratch">通配值拼接缓冲，可为 NULL（此时只有不消耗请求段的末尾通配能命中）</param>
/// <param name="scratch_cap">scratch 字节数</param>
/// <param name="method">请求方法位掩码</param>
/// <param name="out">匹配结果；命中时 params 的值指向 segs 或 scratch</param>
/// <returns>≥0=命中 route id；-1=out 为 NULL、参数非法或无路径；-3=方法掩码为 0；-4=路径在而方法不对；
/// -5=路径在、请求是 OPTIONS 且没有 OPTIONS handler（自动应答）</returns>
int32_t rtree_match(rtree_ctx *t, const buf_ctx *segs, int32_t nseg, uint8_t *scratch, size_t scratch_cap, router_method method,
                    rtree_match_result *out);
/// <summary>
/// 释放 rtree_add 产出的 variants 数组并清零计数。
/// </summary>
/// <param name="res">注册结果，可为 NULL</param>
void rtree_add_result_free(rtree_add_result *res);
/// <summary>
/// 校验节点 id、字面量 radix 子节点、param/wild 子节点、叶子掩码和 leaf_count。
/// </summary>
/// <param name="t">树</param>
/// <returns>ERR_OK 通过；ERR_FAILED 不变式被破坏</returns>
int32_t rtree_check(const rtree_ctx *t);
/// <summary>
/// 节点数（含根）。
/// </summary>
/// <param name="t">树，可为 NULL</param>
/// <returns>节点数；t 为 NULL 返回 0</returns>
size_t rtree_node_count(const rtree_ctx *t);
/// <summary>
/// 叶子条目数。
/// </summary>
/// <param name="t">树，可为 NULL</param>
/// <returns>叶子条目数；t 为 NULL 返回 0</returns>
size_t rtree_leaf_count(const rtree_ctx *t);
/// <summary>
/// 近似占用字节数：树结构 + 节点数组 + 叶子数组 + 字面量 radix，数组按容量算。
/// </summary>
/// <param name="t">树，可为 NULL</param>
/// <returns>字节数；t 为 NULL 返回 0</returns>
size_t rtree_mem(const rtree_ctx *t);

#endif // ROUTER_TREE_H_
