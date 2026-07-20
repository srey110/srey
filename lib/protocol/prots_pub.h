#ifndef PROTS_PUB_H_
#define PROTS_PUB_H_

#include "base/structs.h"

// mysql pgsql monogo smtp引用宏（ref：0=C 借用，事件层不释放块；>0=Lua 堆持有者数）
// 建连前 acquire：仅 Lua 持有(ref>0)时 +1，C 借用(ref=0)短路
#define PROT_REF_ACQUIRE(ptr) \
    do { \
        if (0 != ATOMIC_GET(&(ptr)->ref)) { \
            ATOMIC_ADD(&(ptr)->ref, 1); \
        } \
    } while (0)
// release：C 借用(ref=0)短路，Lua 持有者归零时 FREE；事件层 udfree 与 Lua __gc 共用(__gc 时 ref 必>0，GET 短路恒真)
#define PROT_REF_RELEASE(ptr) \
    do { \
        if (0 != ATOMIC_GET(&(ptr)->ref) && 1 == ATOMIC_ADD(&(ptr)->ref, -1)) { \
            FREE(ptr); \
        } \
    } while (0)

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
    MSG_TYPE_FORK,          // 内部 mtype 标记：coro_fork/coro_fork_wait 的子任务经 fork_pending 链表，
                            // 在 dispatch 末尾 drain 起协程，_coro_mco_cb 据此路由到 _coro_fork_run（不入消息队列）
    MSG_TYPE_ALL            // 消息类型总数（边界值）
}msg_type;
// 协议包类型枚举
typedef enum pack_type {
    PACK_NONE = 0x00,       // 无协议（透传原始数据）
    PACK_DNS,               // DNS 协议
    PACK_HTTP,              // HTTP 协议
    PACK_WEBSOCK,           // WebSocket 协议
    PACK_MQTT,              // MQTT 协议
    PACK_SMTP,              // SMTP 协议
    PACK_CUSTZ_FIXED,       // 自定义协议 - 固定 4 字节长度头
    PACK_CUSTZ_FLAG,        // 自定义协议 - 标志位变长头
    PACK_CUSTZ_VAR,         // 自定义协议 - MQTT 风格变长头

    PACK_REDIS = 0x20,      // Redis RESP 协议
    PACK_MYSQL,             // MySQL 协议
    PACK_PGSQL,             // PostgreSQL 协议
    PACK_MONGO,             // MongoDB Wire 协议

    PACK_UDP_KCP = 0x40
}pack_type;
// 协议解包状态标志（可多个标志同时置位）
typedef enum prot_status {
    PROT_INIT = 0x00,          // 初始/正常状态
    PROT_SLICE_START = 0x01,   // 分片起始包
    PROT_SLICE = 0x02,         // 分片中间包
    PROT_SLICE_END = 0x04,     // 分片结束包
    PROT_ERROR = 0x08,         // 协议错误
    PROT_MOREDATA = 0x10,      // 数据不足，需等待更多数据
    PROT_CLOSE = 0x20          // 连接关闭信号
}prot_status;

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
// 握手完成后的推送回调函数类型
typedef int32_t(*_handshaked_push)(SOCKET fd, uint64_t skid, int32_t client,
    ud_cxt *ud, int32_t erro, void *data, size_t lens);
// 消息汇：网络事件回调向上推消息的接口，由 task 层注册实现
typedef void*(*prots_emit_begin_cb)(void *loader, name_t handle);// 开窗：grab 目标，返回不透明句柄，NULL=目标不存在
typedef void(*prots_emit_cb)(void *target, message_ctx *msg);// 推一条消息给已开窗的目标
typedef void(*prots_emit_end_cb)(void *target);              // 关窗：释放 begin 取得的句柄
typedef struct prot_emit {
    prots_emit_begin_cb begin;
    prots_emit_cb emit;
    prots_emit_end_cb end;
}prot_emit;

#endif// PROTS_PUB_H_
