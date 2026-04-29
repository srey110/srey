#ifndef STARTUP_H_
#define STARTUP_H_

#include "lib.h"
#if WITH_LUA
#include "lbind/ltask.h"
#endif

// 服务配置结构体，由 config.json 解析填充
typedef struct config_ctx {
    uint8_t  loglv;            // 日志级别
    uint16_t nnet;             // 网络线程数，0 表示使用 CPU 核心数
    uint16_t nworker;          // 工作线程数，0 表示使用 CPU 核心数
    uint16_t harborport;       // harbor 监听端口
    uint32_t stacksize;        // 协程栈大小（字节），0 使用默认值
    uint32_t twqueuelens;      // 时间轮队列大小 0 使用默认值 4096
    uint32_t logqueuelens;     // 日志队列大小 0 使用默认值 4096
    name_t   harborname;       // harbor 任务名
    name_t   harborssl;        // harbor SSL 任务名（0 表示不启用 SSL）
    char dns[IP_LENS];         // DNS 服务器地址
    char harborip[IP_LENS];    // harbor 监听 IP
    char harborkey[128];       // harbor 通信密钥
    char script[PATH_LENS];    // Lua 脚本入口目录（WITH_LUA 时有效）
}config_ctx;

/// <summary>
/// 启动业务任务（harbor 及可选的 Lua 任务）
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="config">服务配置</param>
/// <returns>ERR_OK 成功</returns>
static int32_t task_startup(loader_ctx *loader, config_ctx *config) {
    int32_t rtn = harbor_start(loader, config->harborname, config->harborssl,
        config->harborip, config->harborport, config->harborkey);
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
};

#endif//STARTUP_H_
