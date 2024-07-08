#ifndef TASK_H_
#define TASK_H_

#include "srey/spub.h"

typedef enum task_netev {
    NETEV_NONE = 0x00,
    NETEV_ACCEPT = 0x01,
    NETEV_AUTHSSL = 0x02,
    NETEV_SEND = 0x04
}task_netev;

task_ctx *task_new(loader_ctx *loader, name_t name, _task_dispatch_cb _dispatch, free_cb _argfree, void *arg);
void task_free(task_ctx *task);
int32_t task_register(task_ctx *task, _task_startup_cb _startup, _task_closing_cb _closing);
void task_close(task_ctx *task);//任务关闭
task_ctx *task_grab(loader_ctx *loader, name_t name);
void task_incref(task_ctx *task);
void task_ungrab(task_ctx *task);//与 task_grab task_incref 配对

void task_timeout(task_ctx *task, uint64_t sess, uint32_t ms, _timeout_cb _timeout);
void task_request(task_ctx *dst, task_ctx *src, uint8_t reqtype, uint64_t sess, void *data, size_t size, int32_t copy);
void task_response(task_ctx *dst, uint64_t sess, int32_t erro, void *data, size_t size, int32_t copy);
void task_call(task_ctx *dst, uint8_t reqtype, void *data, size_t size, int32_t copy);
int32_t task_listen(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, uint64_t *id, int32_t netev);
SOCKET task_connect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, uint64_t *skid, int32_t netev);
SOCKET task_conn_extra(task_ctx *task, pack_type pktype, void *extra,
    const char *ip, uint16_t port, uint64_t *skid, int32_t netev);
SOCKET task_udp(task_ctx *task, const char *ip, uint16_t port, uint64_t *skid);

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
