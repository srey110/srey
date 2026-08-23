#ifndef PATH_RULES_H_
#define PATH_RULES_H_

#include "base/macro.h"

// 这两个值必须与 protocol/urlparse.h 的 URL_MAX_PATH_DEPTH / URL_BUF_LENS 保持一致:
// 同一个 HTTP 路径先按 URL_* 切段再进这里校验, 只调一边会出现"解析过了校验拒绝"
#define PATH_MAX_DEPTH  64 // 单条路径的段数上限
#define PATH_BUF_LENS   ONEK // 路径字符串工作缓冲(insert 侧据此限长, path_scan 据此重建)

typedef enum path_kind {
    PATH_KIND_LITERAL  = 0,   // 精确路径,不含通配
    PATH_KIND_WILDCARD = 1,   // 含通配的订阅模式
}path_kind;
typedef struct path_rules {
    char sep;                                       // 段分隔符(必填,如 '/')
    char single_wildcard;                           // 单层通配字符(0 = 禁用)
    char multi_wildcard;                            // 多层通配字符(0 = 禁用,启用时只允许在末尾出现)
    // 段级扩展校验(NULL = 仅内置基础校验)
    int32_t (*validate_segment)(const char *seg, size_t len, path_kind kind, void *udata);
    // 路径级扩展校验(NULL = 跳过)
    int32_t (*validate_path)(const char *path, path_kind kind, void *udata);
    void *udata;                                    // 透传给两个回调
}path_rules;

/// <summary>
/// 填充通用 pub/sub 规则:sep='/', single_wildcard='+', multi_wildcard='#'。
/// 内置校验(由 path_trie):'#' 必须末尾、'+'/'#' 独占段、段非空。
/// 无协议特定额外约束;填充后可追加 validate_segment / validate_path 自定义校验。
/// </summary>
/// <param name="rule">输出参数:被填充的 path_rules 结构(调用方持有,不可 NULL)</param>
void path_rules_def(path_rules *rule);
/// <summary>
/// 填充 MQTT 风格规则:同 path_rules_def,
/// 额外加 MQTT 协议特定校验:订阅模式不允许以 '$' 开头(MQTT v3.1.1 §4.7.2 系统 topic 保留)。
/// </summary>
/// <param name="rule">输出参数:被填充的 path_rules 结构(调用方持有,不可 NULL)</param>
void path_rules_mqtt(path_rules *rule);

#endif//PATH_RULES_H_
