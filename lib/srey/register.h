#ifndef REGISTER_H_
#define REGISTER_H_

#include "srey/spub.h"

static inline void on_accepted(task_ctx *task, _net_accept_cb _accept) {
    task->_net_accept = _accept;
}
static inline void on_recved(task_ctx *task, _net_recv_cb _recv) {
    task->_net_recv = _recv;
}
static inline void on_sended(task_ctx *task, _net_send_cb _send) {
    task->_net_send = _send;
}
static inline void on_connected(task_ctx *task, _net_connect_cb _connect) {
    task->_net_connect = _connect;
}
static inline void on_ssl_exchanged(task_ctx *task, _net_ssl_exchanged_cb _exchanged) {
    task->_ssl_exchanged = _exchanged;
}
static inline void on_handshaked(task_ctx *task, _net_handshake_cb _handshake) {
    task->_net_handshaked = _handshake;
}
static inline void on_closed(task_ctx *task, _net_close_cb _close) {
    task->_net_close = _close;
}
static inline void on_recvedfrom(task_ctx *task, _net_recvfrom_cb _recvfrom) {
    task->_net_recvfrom = _recvfrom;
}
static inline void on_requested(task_ctx *task, _request_cb _request) {
    task->_request = _request;
}
static inline void on_responsed(task_ctx *task, _response_cb _response) {
    task->_response = _response;
}

#endif//
