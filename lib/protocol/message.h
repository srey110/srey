#ifndef MESSAGE_H_
#define MESSAGE_H_

#include "base/structs.h"

// 任务间消息类型枚举
typedef enum msg_type {
    MSG_TYPE_NONE = 0x00,   // 无消息（占位）
    MSG_TYPE_STARTUP,       // 任务启动
    MSG_TYPE_CLOSING,       // 任务关闭
    MSG_TYPE_TIMEOUT,       // 超时
    MSG_TYPE_ACCEPT,        // 新 TCP 连接接受
    MSG_TYPE_CONNECT,       // TCP 主动连接建立
    MSG_TYPE_SSLEXCHANGED,  // SSL 握手完成
    MSG_TYPE_HANDSHAKED,    // 应用层握手完成
    MSG_TYPE_RECV,          // TCP 数据接收
    MSG_TYPE_SEND,          // TCP 数据发送完成
    MSG_TYPE_CLOSE,         // 连接关闭
    MSG_TYPE_RECVFROM,      // UDP 数据接收
    MSG_TYPE_REQUEST,       // 任务间请求
    MSG_TYPE_RESPONSE,      // 任务间响应
    MSG_TYPE_FORK,          // 内部 mtype：由 coro_fork 自发投递到本 task 的消息队列，
                            // 业务回调跑在 _handle_fork 内（与 REQUEST 同模式：每条消息总是新协程）
    MSG_TYPE_ALL            // 消息类型总数（边界值）
}msg_type;
// 任务间传递的消息体
typedef struct message_ctx {
    uint8_t slice;  // 分片类型（slice_type）
    uint8_t client; // 1 表示客户端连接，0 表示服务端连接
    subtype_t subtype; // 数据包解包类型（pack_type）或 请求类型（request_type）
    msg_type mtype;  // 消息类型
    int32_t erro;   // 错误码
    size_t size;    // 数据长度
    name_t src;     // 发送方任务名
    uint64_t sess;  // 会话 ID（用于请求/响应匹配）
    void *data;     // 消息数据指针
    shared_data *shared; // NULL=独占（默认 _message_clean 走 prots_pkfree/FREE）；非 NULL=task_multi_call / task_multi_request 广播,N 个 task 共享同一 data,各 task 释放时 ATOMIC_ADD(&ref,-1) 归 0 才 FREE
    sk_id sk;       // 连接标识 fd+skid
}message_ctx;

#endif//MESSAGE_H_
