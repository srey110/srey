#ifndef TASK_H_
#define TASK_H_

#include "srey/spub.h"

task_ctx *task_new(loader_ctx *loader, name_t name, _task_dispatch_cb _dispatch, free_cb _argfree, void *arg);
void task_free(task_ctx *task);
int32_t task_register(task_ctx *task, _task_startup_cb _startup, _task_closing_cb _closing);
void task_close(task_ctx *task);//任务关闭
task_ctx *task_grab(loader_ctx *loader, name_t name);
void task_incref(task_ctx *task);
void task_ungrab(task_ctx *task);//与 task_grab task_incref 配对

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
