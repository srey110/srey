#ifndef PATH_RULES_H_
#define PATH_RULES_H_

#include "base/macro.h"

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

// path_rules 预设填充器
// 为常见协议/场景提供开箱即用的 path_rules 填充函数。
// 调用方持有 path_rules 内存,调函数填充字段后传地址给 path_new(&rules, _free)
// 或服务注册函数。填充后可追加自定义 validate_segment / validate_path 回调扩展。
// 当前提供:
//   path_rules_def   通用 pub/sub:'/' 分隔、'+' 单层、'#' 多层
//                    仅 path_trie 内置基础校验,无协议特定约束。
//   path_rules_mqtt  MQTT 风格主题:同 def,额外加 '$' 前缀禁订阅校验。

/// <summary>
/// 填充通用 pub/sub 规则:sep='/', single_wildcard='+', multi_wildcard='#'。
/// 内置校验(由 path_trie):'#' 必须末尾、'+'/'#' 独占段、段非空、不含 sep / NUL。
/// 无协议特定额外约束。填充后可追加 validate_segment / validate_path 自定义校验。
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
