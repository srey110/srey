#ifndef PATH_TRIE_H_
#define PATH_TRIE_H_

#include "base/structs.h"
#include "path/path_rules.h"

// path_trie:协议无关的分层路径前缀树,支持通配匹配。
// 业务方通过 path_rules 注入分隔符 / 通配字符 / 自定义校验,
// 预设见 path/path_rules.h(path_rules_def / path_rules_mqtt)。

typedef void (*match_visit_cb)(void *payload, void *udata);
typedef void (*scan_visit_cb)(const char *path, void *payload, void *udata);
typedef struct path_trie path_trie;

/// <summary>独立校验路径合法性(不修改 trie 状态)</summary>
/// <param name="rules">规则配置;NULL 返 ERR_FAILED</param>
/// <param name="path">待校验路径;NULL 返 ERR_FAILED</param>
/// <param name="kind">PATH_KIND_LITERAL 拒绝通配字符;PATH_KIND_WILDCARD 允许通配</param>
/// <returns>ERR_OK 合法;ERR_FAILED 非法或参数错误</returns>
int32_t path_validate(const path_rules *rules, const char *path, path_kind kind);
/// <summary>判断精确路径是否匹配含通配的订阅模式</summary>
/// <param name="rules">规则配置(用于 sep / wildcard 字符)</param>
/// <param name="literal_path">精确路径,不含通配</param>
/// <param name="pattern">订阅模式,可含通配</param>
/// <returns>ERR_OK 匹配;ERR_FAILED 不匹配或参数非法</returns>
int32_t path_matches_pattern(const path_rules *rules,
                             const char *literal_path,
                             const char *pattern);
/// <summary>
/// 创建 trie。rules 由调用方持有,trie 只存指针不拷贝,生命周期需 ≥ trie。
/// </summary>
/// <param name="rules">规则配置(必填;sep=0 返 NULL)</param>
/// <param name="_path_free">payload 释放回调(path_free 与同 path 二次 insert 时调用,可 NULL)</param>
/// <returns>trie 指针;参数非法或分配失败返 NULL</returns>
path_trie *path_new(const path_rules *rules, free_cb _path_free);
/// <summary>释放 trie,遍历所有 payload 调 _free,然后销毁全部节点</summary>
/// <param name="t">trie 指针;NULL 安全</param>
void path_free(path_trie *t);
/// <summary>当前 payload 节点数(不含纯路径中间节点)</summary>
/// <param name="t">trie 指针;NULL 返 0</param>
/// <returns>payload 节点总数</returns>
size_t path_count(const path_trie *t);
/// <summary>
/// 插入 payload。所有权转 trie;同 path 已存在则旧 payload 通过 _free 释放。
/// 路径走 WILDCARD 校验,允许含通配的订阅模式。
/// </summary>
/// <param name="t">trie 指针</param>
/// <param name="path">路径字符串;路径非法返 ERR_FAILED 且 payload 由调用方自行释放</param>
/// <param name="payload">业务数据;不可为 NULL,传 NULL 返 ERR_FAILED</param>
/// <returns>ERR_OK 成功;ERR_FAILED payload 为 NULL/参数非法/路径非法</returns>
int32_t path_insert(path_trie *t, const char *path, void *payload);
/// <summary>精确查找 payload</summary>
/// <param name="t">trie 指针</param>
/// <param name="path">精确路径(允许含通配字符,作为字面段查找)</param>
/// <returns>payload 指针;不存在或路径非法返 NULL</returns>
void *path_get(path_trie *t, const char *path);
/// <summary>
/// 取或创建:不存在时塞入 init 并返回 init;已存在时返回现有 payload,init 不被使用,
/// 由调用方自行释放 init。init 为 NULL 时仅查询不创建,等同 path_get。
/// </summary>
/// <param name="t">trie 指针</param>
/// <param name="path">路径字符串</param>
/// <param name="init">不存在时塞入的初始 payload(允许 NULL,此时纯查询)</param>
/// <returns>最终 payload 指针;参数非法返 NULL</returns>
void *path_get_or_create(path_trie *t, const char *path, void *init);
/// <summary>
/// 删除并返回原 payload(所有权交回业务,trie 不再持有,_free 不被调用)。
/// 空中间节点沿父链自动回收。
/// </summary>
/// <param name="t">trie 指针</param>
/// <param name="path">路径字符串</param>
/// <returns>原 payload 指针;不存在或路径非法返 NULL</returns>
void *path_remove(path_trie *t, const char *path);
/// <summary>
/// 通配匹配:literal_path 必须是精确路径(LITERAL 校验拒绝通配字符)。
/// cb 可能对同一 payload 触发多次(同一订阅者多 pattern 命中),业务自行去重。
/// </summary>
/// <param name="t">trie 指针</param>
/// <param name="literal_path">精确路径;含通配字符则直接返回不调 cb</param>
/// <param name="cb">每个匹配 payload 调用一次;NULL 直接返回</param>
/// <param name="udata">透传给 cb 的上下文</param>
void path_match(path_trie *t, const char *literal_path, match_visit_cb cb, void *udata);
/// <summary>
/// 全遍历所有 payload 节点。cb 收到完整路径字符串(实时重建,栈缓冲 PATH_SCAN_BUF=1024)。
/// 路径过长的节点跳过 + LOG_WARN(insert 已限长,实际不会触发)。
/// </summary>
/// <param name="t">trie 指针</param>
/// <param name="cb">每个 payload 节点调用一次;NULL 直接返回</param>
/// <param name="udata">透传给 cb 的上下文</param>
void path_scan(path_trie *t, scan_visit_cb cb, void *udata);

#endif//PATH_TRIE_H_
