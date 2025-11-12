#ifndef CORO_H_
#define CORO_H_

#include "srey/spub.h"

/// <summary>
/// 休眠
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="ms">毫秒</param>
void coro_sleep(task_ctx *task, uint32_t ms);
/// <summary>
/// 任务间通信 请求
/// </summary>
/// <param name="dst">目标任务</param>
/// <param name="src">发起者</param>
/// <param name="rtype">请求类型</param>
/// <param name="data">数据</param>
/// <param name="size">数据长度</param>
/// <param name="copy">1 拷贝数据 0 不拷贝数据</param>
/// <param name="erro">错误码</param>
/// <param name="lens">返回数据长度</param>
/// <returns>数据</returns>
void *coro_request(task_ctx *dst, task_ctx *src, uint8_t rtype, void *data, size_t size, int32_t copy, int32_t *erro, size_t *lens);
/// <summary>
/// 切换为SSL链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="client">1 作为客户端 0 作为服务端</param>
/// <param name="evssl">evssl_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t coro_ssl_exchange(task_ctx *task, SOCKET fd, uint64_t skid, int32_t client, struct evssl_ctx *evssl);
/// <summary>
/// 等待握手完成
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="err">错误码</param>
/// <param name="size">返回数据长度</param>
/// <returns>数据</returns>
void *coro_handshaked(task_ctx *task, SOCKET fd, uint64_t skid, int32_t *err, size_t *size);
/// <summary>
/// 链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="pktype">数据包类型</param>
/// <param name="evssl">evssl_ctx</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="skid">链接ID</param>
/// <param name="netev">task_netev</param>
/// <returns>socket句柄</returns>
SOCKET coro_connect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl, const char *ip, uint16_t port, uint64_t *skid, int32_t netev);
/// <summary>
/// TCP发送
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="data">数据</param>
/// <param name="len">数据长度</param>
/// <param name="size">返回数据长度</param>
/// <param name="copy">1 拷贝数据 0 不拷贝</param>
/// <returns>数据</returns>
void *coro_send(task_ctx *task, SOCKET fd, uint64_t skid, void *data, size_t len, size_t *size, int32_t copy);
/// <summary>
/// 等待分片消息
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="size">数据长度</param>
/// <param name="end">1 分片结束 0未结束</param>
/// <returns>数据</returns>
void *coro_slice(task_ctx *task, SOCKET fd, uint64_t skid, size_t *size, int32_t *end);
/// <summary>
/// UDP发送
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="data">数据</param>
/// <param name="len">数据长度</param>
/// <param name="size">返回数据长度</param>
/// <returns>数据</returns>
void *coro_sendto(task_ctx *task, SOCKET fd, uint64_t skid, const char *ip, const uint16_t port, void *data, size_t len, size_t *size);

#endif//CORO_H_
