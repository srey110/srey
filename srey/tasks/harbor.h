#ifndef HARBOR_H_
#define HARBOR_H_

#include "lib.h"

/// <summary>
/// 启动harbor,服务器间通信
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="tname">任务名</param>
/// <param name="ssl">evssl_ctx 名称</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="key">密钥</param>
/// <param name="ms">任务间通信超时时间 毫秒</param>
/// <returns>ERR_OK 成功</returns>
int32_t harbor_start(loader_ctx *loader, name_t tname, name_t ssl,
    const char *ip, uint16_t port, const char *key, int32_t ms);
/// <summary>
/// 服务器间通信请求包
/// </summary>
/// <param name="task">目标任务名</param>
/// <param name="call">1 执行call 0 执行request</param>
/// <param name="reqtype">请求类型</param>
/// <param name="key">密钥</param>
/// <param name="data">数据</param>
/// <param name="size">数据长度</param>
/// <param name="lens">请求包长度</param>
/// <returns>请求包</returns>
void *harbor_pack(name_t task, int32_t call, uint8_t reqtype, const char *key, void *data, size_t size, size_t *lens);

#endif//HARBOR_H_
