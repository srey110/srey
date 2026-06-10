#include "path/path_rules.h"

// 通用 pub/sub:仅 path_trie 内置基础校验,无协议特定额外约束
void path_rules_def(path_rules *rule) {
    ZERO(rule, sizeof(path_rules));
    rule->sep = '/';
    rule->single_wildcard = '+';
    rule->multi_wildcard = '#';
}
// MQTT 协议特定:订阅模式不允许以 '$' 开头($SYS 等系统 topic 保留 broker)
// 注:发布精确 topic 同样不允许以 '$' 开头(客户端不应触碰系统 topic)
static int32_t _path_rules_mqtt_validate(const char *path, path_kind kind, void *udata) {
    (void)kind;
    (void)udata;
    if ('$' == path[0]) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
void path_rules_mqtt(path_rules *rule) {
    ZERO(rule, sizeof(path_rules));
    rule->sep = '/';
    rule->single_wildcard = '+';
    rule->multi_wildcard = '#';
    rule->validate_path = _path_rules_mqtt_validate;
}
