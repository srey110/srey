#include "startup.h"

int32_t task_startup(loader_ctx *loader, config_ctx *config) {
    // DataCenter task service:必须早于业务 task 启动,否则业务 coro_dc_* 找不到 dst;
    // datacenter.name 空串时 dc_start 直接跳过不启动
    int32_t rtn = dc_start(loader, config->datacenter.name);
    if (ERR_OK != rtn) {
        return rtn;
    }
    // subcenter task service:与 DataCenter 同期启动;subcenter.name 空串时 sc_start 直接跳过
    // 规则由 config subcenter.rule 选择:"mqtt" 走 MQTT 风格,其余走 path_rules_def(通用 pub/sub)
    // static 让 sc_rules 生命周期 = 程序,subcenter 内部只存指针不拷贝
    static path_rules sc_rules;
    if (0 == strcmp(config->subcenter.rule, "mqtt")) {
        path_rules_mqtt(&sc_rules);
    } else {
        path_rules_def(&sc_rules);
    }
    rtn = sc_start(loader, config->subcenter.name, &sc_rules);
    if (ERR_OK != rtn) {
        return rtn;
    }
    rtn = harbor_start(loader, config->harbor.name, config->harbor.ssl,
        config->harbor.ip, config->harbor.port, config->harbor.key);
    if (ERR_OK != rtn) {
        return rtn;
    }
    // debug_console 调试控制台:debug.port 0 / debug.name 空串时 debug_console_start 跳过
    rtn = debug_console_start(loader, config->debug.name, config->debug.ip, config->debug.port);
    if (ERR_OK != rtn) {
        return rtn;
    }
#if WITH_LUA
    rtn = ltask_startup(config->script);
    if (ERR_OK != rtn) {
        return rtn;
    }
#endif
    return rtn;
}
