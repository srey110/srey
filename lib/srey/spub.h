#ifndef SVPUB_H_
#define SVPUB_H_

#include "protocol/prots.h"
#include "event/event.h"
#include "containers/sarray.h"
#include "containers/queue.h"
#include "containers/mpmc.h"
#include "thread/rwlock.h"
#include "thread/spinlock.h"
#include "thread/mutex.h"
#include "thread/cond.h"
#include "utils/tw.h"

#define INVALID_TNAME         0  // 无效任务名（空值）
typedef struct loader_ctx loader_ctx;
typedef struct task_ctx task_ctx;
typedef struct message_ctx message_ctx;
typedef struct task_dispatch_arg task_dispatch_arg;

typedef void(*_task_dispatch_cb)(task_dispatch_arg *arg);           // 消息分发回调
typedef void(*_task_startup_cb)(task_ctx *task);                    // 任务启动回调
typedef void(*_task_closing_cb)(task_ctx *task);                    // 任务关闭回调
typedef void(*_timeout_cb)(task_ctx *task, uint64_t sess);          // 超时回调
typedef void(*_request_cb)(task_ctx *task, uint8_t reqtype, uint64_t sess, name_t src, void *data, size_t size); // 任务请求回调
typedef void(*_response_cb)(task_ctx *task, uint64_t sess, int32_t error, void *data, size_t size);              // 任务响应回调
typedef void(*_net_accept_cb)(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype);                         // 新连接接受回调
typedef void(*_net_recv_cb)(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, uint8_t slice, void *data, size_t size); // 数据接收回调
typedef void(*_net_send_cb)(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, size_t size);  // 数据发送完成回调
typedef void(*_net_connect_cb)(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, int32_t erro);              // 连接建立回调
typedef void(*_net_ssl_exchanged_cb)(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client);      // SSL 交换完成回调
typedef void(*_net_handshake_cb)(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, int32_t erro, void *data, size_t lens); // 应用层握手完成回调
typedef void(*_net_close_cb)(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client);              // 连接关闭回调
typedef void(*_net_recvfrom_cb)(task_ctx *task, SOCKET fd, uint64_t skid, char ip[IP_LENS], uint16_t port, void *data, size_t size); // UDP 数据接收回调

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

    MSG_TYPE_ALL            // 消息类型总数（边界值）
}msg_type;
// 任务间传递的消息体
struct message_ctx {
    uint8_t mtype;  // 消息类型（msg_type）
    uint8_t pktype; // 数据包解包类型（pack_type）
    uint8_t slice;  // 分片类型（slice_type）
    uint8_t client; // 1 表示客户端连接，0 表示服务端连接
    int32_t erro;   // 错误码
    name_t src;     // 发送方任务名
    SOCKET fd;      // socket 句柄
    void *data;     // 消息数据指针
    size_t size;    // 数据长度
    uint64_t skid;  // 连接 ID
    uint64_t sess;  // 会话 ID（用于请求/响应匹配）
};
// 工作线程版本快照，供监控线程检测卡死
typedef struct worker_version {
    uint8_t msgtype;  // 当前正在处理的消息类型
    int32_t ckver;    // 上次检查时记录的版本号
    int32_t ver;      // 当前处理消息计数版本号
    name_t name;      // 当前正在处理的任务名
}worker_version;
// 监控线程上下文
typedef struct monitor_ctx {
    uint8_t stop;               // 非 0 表示监控线程应退出
    worker_version *version;    // 各工作线程的版本快照数组
    pthread_t thread_monitor;   // 监控线程句柄
}monitor_ctx;
// 工作线程上下文
typedef struct worker_ctx {
    uint16_t index;        // 工作线程索引
    int32_t weight;        // 批处理权重（-1=单条 0=全量 >0=按比例）
    atomic_t waiting;      // 当前是否在休眠等待（原子维护，快速路径无锁读）
    loader_ctx *loader;    // 所属 loader
    pthread_t thread_worker; // 工作线程句柄
    mpmc_ctx qutasks;      // 无锁任务名队列（替代原 spinlock + qu_task）
    mutex_ctx mutex;       // 配合条件变量使用的互斥锁
    cond_ctx cond;         // 工作线程休眠/唤醒条件变量
}worker_ctx;
// 任务调度器全局上下文
struct loader_ctx {
    uint8_t stop;              // 非 0 表示调度器应停止
    uint16_t nworker;          // 工作线程数量
    int32_t waiting;           // （保留字段）
    atomic64_t index;          // 轮询工作线程的原子计数器
    worker_ctx *worker;        // 工作线程数组
    struct hashmap *maptasks;  // 任务名 → task_ctx 的哈希映射
    rwlock_ctx lckmaptasks;    // 保护 maptasks 的读写锁
    monitor_ctx monitor;       // 监控线程上下文
    tw_ctx tw;                 // 时间轮（超时调度）
    ev_ctx netev;              // 网络事件驱动上下文
};
// 任务上下文
struct task_ctx {
    atomic_t global;           // 0=未调度 1=已调度/运行中（无锁调度标志）
    name_t name;               // 任务唯一名称
    uint32_t overload;         // 消息队列积压告警阈值（动态翻倍）
    atomic_t closing;          // 是否已发送关闭消息（防重复）
    atomic_t ref;              // 引用计数
    uint32_t timeout_request;  // task_request 超时时间（毫秒）
    uint32_t timeout_connect;  // task_connect 超时时间（毫秒）
    uint32_t timeout_netread;  // 网络读取超时时间（毫秒）
    void *arg;                 // 用户自定义数据
    free_cb _arg_free;         // 用户数据释放回调
    loader_ctx *loader;        // 所属 loader
    _task_dispatch_cb _task_dispatch;    // 消息分发函数
    _task_startup_cb _task_startup;      // 启动回调
    _task_closing_cb _task_closing;      // 关闭回调
    _net_accept_cb _net_accept;          // 新连接接受回调
    _net_recv_cb _net_recv;              // 数据接收回调
    _net_send_cb _net_send;              // 数据发送完成回调
    _net_connect_cb _net_connect;        // 连接建立回调
    _net_handshake_cb _net_handshaked;   // 应用层握手完成回调
    _net_close_cb _net_close;            // 连接关闭回调
    _net_recvfrom_cb _net_recvfrom;      // UDP 接收回调
    _request_cb _request;                // 任务请求回调
    _response_cb _response;              // 任务响应回调
    _net_ssl_exchanged_cb _ssl_exchanged; // SSL 交换完成回调
    mpmc_ctx qumsg;            // 无锁消息队列（替代原 spinlock + qu_message）
};
// 消息分发时传递给 _task_dispatch 的参数包
struct task_dispatch_arg {
    task_ctx *task;    // 目标任务
    message_ctx msg;   // 消息体（值拷贝）
};

// 根据消息类型调用对应处理函数（内部接口）
void _message_run(task_ctx *task, message_ctx *msg);
// 将握手完成消息推入任务队列（由协议层回调，内部接口）
int32_t _message_handshaked_push(SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud, int32_t erro, void *data, size_t lens);
// 将消息推入任务的无锁消息队列（内部接口）
void _task_message_push(task_ctx *task, message_ctx *msg);
// 判断消息是否需要清理数据（内部接口）
int32_t _message_should_clean(message_ctx *msg);
// 根据消息类型释放消息数据（内部接口）
void _message_clean(msg_type mtype, pack_type pktype, void *data);

#endif//SVPUB_H_
