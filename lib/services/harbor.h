#ifndef HARBOR_H_
#define HARBOR_H_

#include "srey/coro.h"

/// <summary>
/// 启动harbor,服务器间通信
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="tname">字符串任务名；NULL 或空串表示不启动 harbor</param>
/// <param name="ssl">evssl_ctx 名称, NULL 或 "" 表示不启用 SSL</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="key">密钥</param>
/// <returns>ERR_OK 成功</returns>
int32_t harbor_start(loader_ctx *loader, const char *tname, const char *ssl, const char *ip, uint16_t port, const char *key);
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
/// <returns>请求包;key 非空但签名生成失败返 NULL 且 *lens=0</returns>
void *harbor_pack(name_t task, int32_t call, uint8_t reqtype, const char *key, void *data, size_t size, size_t *lens);

#endif//HARBOR_H_
