#ifndef TASK_H_
#define TASK_H_

#include "srey/spub.h"

typedef enum task_netev {
    NETEV_NONE = 0x00,
    NETEV_ACCEPT = 0x01,
    NETEV_AUTHSSL = 0x02,
    NETEV_SEND = 0x04
}task_netev;
/// <summary>
/// 新建任务
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="name">任务名</param>
/// <param name="_dispatch">消息分发函数, NULL默认分发函数</param>
/// <param name="_argfree">用户参数释放函数</param>
/// <param name="arg">用户参数</param>
/// <returns>task_ctx</returns>
task_ctx *task_new(loader_ctx *loader, name_t name, _task_dispatch_cb _dispatch, free_cb _argfree, void *arg);
/// <summary>
/// 任务释放
/// </summary>
/// <param name="task">task_ctx</param>
void task_free(task_ctx *task);
/// <summary>
/// 任务注册
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="_startup">任务初始化回调函数</param>
/// <param name="_closing">任务释放回调函数</param>
/// <returns>ERR_OK 成功</returns>
int32_t task_register(task_ctx *task, _task_startup_cb _startup, _task_closing_cb _closing);
/// <summary>
/// 任务关闭 MSG_TYPE_CLOSING
/// </summary>
/// <param name="task">task_ctx</param>
void task_close(task_ctx *task);
/// <summary>
/// 获取任务,并使引用加一
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="name">任务名</param>
/// <returns>task_ctx NULL未找到任务</returns>
task_ctx *task_grab(loader_ctx *loader, name_t name);
/// <summary>
/// 任务引用加一
/// </summary>
/// <param name="task">task_ctx</param>
void task_incref(task_ctx *task);
/// <summary>
/// 释放task_grab获取的任务,并使引用减一. 与 task_grab task_incref 配对
/// </summary>
/// <param name="task">task_ctx</param>
void task_ungrab(task_ctx *task);
/// <summary>
/// 超时
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="sess">session</param>
/// <param name="ms">毫秒</param>
/// <param name="_timeout">超时回调函数</param>
void task_timeout(task_ctx *task, uint64_t sess, uint32_t ms, _timeout_cb _timeout);
/// <summary>
/// 任务间通信,请求
/// </summary>
/// <param name="dst">目标task_ctx</param>
/// <param name="src">发起task_ctx</param>
/// <param name="reqtype">类型</param>
/// <param name="sess">session</param>
/// <param name="data">数据</param>
/// <param name="size">数据长度</param>
/// <param name="copy">1 拷贝 0不拷贝</param>
void task_request(task_ctx *dst, task_ctx *src, uint8_t reqtype, uint64_t sess, void *data, size_t size, int32_t copy);
/// <summary>
/// 任务间通信,返回
/// </summary>
/// <param name="dst">目标task_ctx</param>
/// <param name="sess">session</param>
/// <param name="erro">错误码</param>
/// <param name="data">数据</param>
/// <param name="size">数据长度</param>
/// <param name="copy">1 拷贝 0不拷贝</param>
void task_response(task_ctx *dst, uint64_t sess, int32_t erro, void *data, size_t size, int32_t copy);
/// <summary>
/// 任务间通信,无返回
/// </summary>
/// <param name="dst">目标task_ctx</param>
/// <param name="reqtype">类型</param>
/// <param name="data">数据</param>
/// <param name="size">数据长度</param>
/// <param name="copy">1 拷贝 0不拷贝</param>
void task_call(task_ctx *dst, uint8_t reqtype, void *data, size_t size, int32_t copy);
/// <summary>
/// 监听
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="pktype">包类型</param>
/// <param name="evssl">evssl_ctx</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="id">监听ID</param>
/// <param name="netev">task_netev</param>
/// <returns>ERR_OK 成功</returns>
int32_t task_listen(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, uint64_t *id, int32_t netev);
/// <summary>
/// 链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="pktype">包类型</param>
/// <param name="evssl">evssl_ctx</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="skid">链接ID</param>
/// <param name="netev">task_netev</param>
/// <returns>socket句柄</returns>
SOCKET task_connect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, uint64_t *skid, int32_t netev);
/// <summary>
/// 链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="pktype">包类型</param>
/// <param name="extra">ud_cxt extra</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="skid">链接ID</param>
/// <param name="netev">task_netev</param>
/// <returns>socket句柄</returns>
SOCKET task_conn_extra(task_ctx *task, pack_type pktype, void *extra,
    const char *ip, uint16_t port, uint64_t *skid, int32_t netev);
/// <summary>
/// UDP
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="skid">链接ID</param>
/// <returns>socket句柄</returns>
SOCKET task_udp(task_ctx *task, const char *ip, uint16_t port, uint64_t *skid);
//注册回调
void on_accepted(task_ctx *task, _net_accept_cb _accept);
void on_recved(task_ctx *task, _net_recv_cb _recv);
void on_sended(task_ctx *task, _net_send_cb _send);
void on_connected(task_ctx *task, _net_connect_cb _connect);
void on_ssl_exchanged(task_ctx *task, _net_ssl_exchanged_cb _exchanged);
void on_handshaked(task_ctx *task, _net_handshake_cb _handshake);
void on_closed(task_ctx *task, _net_close_cb _close);
void on_recvedfrom(task_ctx *task, _net_recvfrom_cb _recvfrom);
void on_requested(task_ctx *task, _request_cb _request);
void on_responsed(task_ctx *task, _response_cb _response);

#endif//TASK_H_
