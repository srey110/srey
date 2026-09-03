#include "startup.h"

int32_t task_startup(loader_ctx *loader, config_ctx *config) {
#if WITH_LUA && ENABLE_LUA_BYTECACHE
    // 须排在 ltask_startup 之前:它经 lbc_install_searcher / lbc_loadfile 直接读缓存,没有判空
    lbc_init(loader_lckcache(loader));
#endif
    // debug_console 调试控制台:debug.port 0 / debug.name 空串时 debug_console_start 跳过
    int32_t rtn = debug_console_start(loader, config->debug.name, config->debug.ip, config->debug.port);
    if (ERR_OK != rtn) {
        return rtn;
    }
#if WITH_LUA
    rtn = ltask_startup(config->script);
    if (ERR_OK != rtn) {
        return rtn;
    }
#endif
    // 必须排在 ltask_startup 之后:harbor.ssl 那个名字要往 evssl 注册表里查,而唯一的注册入口
    // 是 startup.lua 顶层调的 core.cert_register / p12_register,ltask_startup 跑完才存在
    rtn = harbor_start(loader, config->harbor.name, config->harbor.ssl,
        config->harbor.ip, config->harbor.port);
    if (ERR_OK != rtn) {
        return rtn;
    }
    return rtn;
}
void task_cleanup(void) {
#if WITH_LUA && ENABLE_LUA_BYTECACHE
    lbc_free();
#endif
}
