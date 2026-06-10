#ifndef STARTUP_H_
#define STARTUP_H_

#include "lib.h"
#if WITH_LUA
#include "lbind/ltask.h"
#endif

#define TASK_NAME_LEN 64           // 系统服务任务名最大长度（config_ctx 内字符串缓冲）

// 服务配置结构体，由 config.json 解析填充
typedef struct config_ctx {
    uint8_t  loglv;            // 日志级别
    uint16_t serviceid;        // 服务器唯一id
    uint16_t nnet;             // 网络线程数，0 表示使用 CPU 核心数
    uint16_t nworker;          // 工作线程数，0 表示使用 CPU 核心数
    uint32_t stacksize;        // 协程栈大小（字节），0 使用默认值
    uint32_t twqueuelens;      // 时间轮队列大小 0 使用默认值 4096
    uint32_t logqueuelens;     // 日志队列大小 0 使用默认值 4096
    char dns[IP_LENS];         // DNS 服务器地址
    char script[PATH_LENS];    // Lua 脚本入口目录（WITH_LUA 时有效）
    struct {                            // 调试控制台（对应 config.json "debug"）
        char name[TASK_NAME_LEN];       // 任务名（"" 表示不启动）
        char ip[IP_LENS];               // 监听 IP
        uint16_t port;                  // 监听端口（0 表示不启动）
    }debug;
    struct {                            // 服务器间通信（对应 config.json "harbor"）
        char name[TASK_NAME_LEN];       // 任务名（"" 表示不启动）
        char ssl[EVSSL_NAME_LEN];       // SSL 名称（"" 表示不启用 SSL）
        char ip[IP_LENS];               // 监听 IP
        uint16_t port;                  // 监听端口
        char key[128];                  // 通信密钥
    }harbor;
    struct {                            // 全局 KV（对应 config.json "datacenter"）
        char name[TASK_NAME_LEN];       // 任务名（"" 表示不启动）
    }datacenter;
    struct {                            // 订阅中心（对应 config.json "subcenter"）
        char name[TASK_NAME_LEN];       // 任务名（"" 表示不启动）
        char rule[16];                  // 规则："def" 通用 pub/sub，"mqtt" MQTT 风格
    }subcenter;
}config_ctx;

/// <summary>
/// 启动业务任务（harbor 及可选的 Lua 任务）
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="config">服务配置</param>
/// <returns>ERR_OK 成功</returns>
int32_t task_startup(loader_ctx *loader, config_ctx *config);

#endif//STARTUP_H_
