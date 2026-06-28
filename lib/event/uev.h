#ifndef UEV_H_
#define UEV_H_

#include "event/event.h"
#include "utils/pool.h"
#include "event/cmds.h"
#include "thread/thread.h"
#include "utils/tda.h"
#include "utils/timer.h"

#ifndef EV_IOCP

// 根据平台选择对应的事件结构体类型
#if defined(EV_EPOLL)
    typedef struct epoll_event events_t;
#elif defined(EV_KQUEUE)
    typedef struct kevent events_t;
    typedef struct kevent changes_t;
    #define COMMIT_NCHANGES// kqueue/devpoll：需批量提交变更列表
#elif defined(EV_EVPORT)
    typedef port_event_t events_t;
    #define MANUAL_ADD// evport：每次触发后需手动重新注册
#elif defined(EV_POLLSET)
    typedef struct pollfd events_t;
    #define MANUAL_REMOVE// pollset：关闭时需手动从pollset中删除
    #define NO_UDATA// pollset/devpoll：事件不携带用户数据，需从hashmap查找
#elif defined(EV_DEVPOLL)
    typedef struct pollfd events_t;
    typedef struct pollfd changes_t;
    #define MANUAL_REMOVE
    #define COMMIT_NCHANGES
    #define NO_UDATA
#endif


// I/O事件类型
typedef enum EVENTS {
    EVENT_READ = 0x01,  // 可读事件
    EVENT_WRITE = 0x02, // 可写事件
}EVENTS;
// 隔离队列元素类型
typedef enum qtn_type {
    QTN_TCP,    // 出队 → pool_push
    QTN_UDP,    // 出队 → _uev_free_udp
    QTN_LSN,    // 出队 → _uev_freelsn
}qtn_type;
// 事件回调函数类型
typedef void(*event_cb)(struct watcher_ctx *watcher, struct sock_ctx *skctx, int32_t ev);
// Unix平台的socket上下文基础结构
typedef struct sock_ctx {
    SOCKET fd;          // socket句柄
    int32_t type;       // socket类型（SOCK_STREAM/SOCK_DGRAM/0表示pipe/listen）
    int32_t events;     // 当前注册的事件掩码
    event_cb ev_cb;     // 事件触发时的回调函数
}sock_ctx;
// 命令管道上下文：一对匿名管道 + 读端的sock_ctx（注册到事件循环）
typedef struct pip_ctx {
    int32_t pipes[2];               // pipes[0] 读端，pipes[1] 写端
    tda_ctx tda;                    // 队列长度告警翻倍状态（init = pipe 容量cmd数 / QUEUE_OVERLOAD_RATIO）
    sock_ctx skpip;                 // 读端的sock_ctx（ev_cb = _uev_cmd_loop）
#if CMD_PIPE_QU
    fsqu_ctx qu;                // 命令队列（多生产者，单消费者批量 pop；元素 cmd_ctx）
#endif
}pip_ctx;
// 隔离队列元素：close 后对象先入此队列暂存 QTN_MS 毫秒，让 stale event 消化完再真释放
typedef struct qtn_entry {
    qtn_type type;
    void *obj;          // sock_ctx * 或 listener_ctx *
    uint64_t enter_ms;  // 入队时刻（monotonic 毫秒）
}qtn_entry;
// 事件监听器上下文（每个工作线程一个）
typedef struct watcher_ctx {
    int32_t index;              // 当前watcher编号
    atomic_t stop;              // 停止标志
    int32_t evfd;               // epoll/kqueue/evport等的事件fd
    int32_t nevents;            // events数组容量
#ifdef COMMIT_NCHANGES
    int32_t nsize;              // changes数组容量
    int32_t nchanges;           // 待提交的变更数量
    changes_t *changes;         // 变更列表（kqueue/devpoll使用）
#endif
    events_t *events;           // 就绪事件数组
    ev_ctx *ev;                 // 所属ev_ctx
    struct hashmap *element;    // fd -> sock_ctx 哈希表
    pthread_t thevent;          // 事件循环线程
    pool_ctx pool;              // sock_ctx对象池
    timer_ctx tm_qtn;           // qtn 用 monotonic 计时
    queue_ctx qtn;              // 隔离队列 FIFO，元素 qtn_entry
    pip_ctx pipe;               // 命令管道（单通道，多生产者 < PIPE_BUF 原子写）
    char udp_rbuf[MAX_RECVFROM_SIZE]; // UDP 接收共享缓冲：本线程所有 UDP socket 复用
}watcher_ctx;

// 向事件多路复用器注册或追加监听事件
int32_t _uev_add_event(watcher_ctx *watcher, SOCKET fd, int32_t *events, int32_t ev, void *arg);
// 从事件多路复用器删除或减少监听事件
void _uev_del_event(watcher_ctx *watcher, SOCKET fd, int32_t *events, int32_t ev, void *arg);

// 在事件循环内完成监听socket的注册
void _uev_add_lsn_inloop(watcher_ctx *watcher, sock_ctx *skctx);
// 在事件循环内取消监听，引用计数归零后释放listener_ctx
void _uev_remove_lsn(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn);
// 尝试对已有连接启动SSL握手（支持延迟到发送完毕）
void _uev_try_ssl_exchange(watcher_ctx *watcher, sock_ctx *skctx, struct evssl_ctx *evssl, int32_t client);
// 在事件循环内将连接中的fd注册可写事件（等待connect完成）
void _uev_add_conn_inloop(watcher_ctx *watcher, sock_ctx *skctx);
// 在事件循环内将accept到的fd完成初始化并注册读事件
void _uev_add_acpfd_inloop(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn);
// 将数据加入发送队列，并确保注册写事件
void _uev_add_write_inloop(watcher_ctx *watcher, sock_ctx *skctx, off_buf_ctx *buf);
// 在事件循环内将socket注册读事件（TCP/UDP通用）
void _uev_add_fd_inloop(watcher_ctx *watcher, sock_ctx *skctx);

// shutdown socket读端（触发关闭流程）
void _uev_sk_shutdown(sock_ctx *skctx);
// 释放UDP socket上下文
void _uev_free_udp(sock_ctx *skctx);
// 标记连接为错误状态并触发关闭（TCP shutdown/UDP注册写事件）
// immed=1 立即关(走 STATUS_ERROR 路径丢弃未发数据)
// immed=0 优雅关(走 STATUS_GRACEFUL_CLOSE 路径,buf_s 发完后 _close_tcp;UDP 退化为立即关)
void _uev_disconnect(watcher_ctx *watcher, sock_ctx *skctx, int32_t immed);
// 释放listener_ctx（立即释放，用于主线程兜底 / worker 退出后 cleanup 路径）
void _uev_freelsn(struct listener_ctx *lsn);
// 递减 listener_ctx 引用计数，归零后立即释放
// 主线程 / drain 残留路径用 (无 _uev_loop_event 在迭代，立即 FREE 安全)
void _uev_try_freelsn(struct listener_ctx *lsn);
// 释放对象入隔离队列 watcher->qtn；QTN_MS 毫秒后由 _uev_qtn_drain 真正释放
// 用于避开 kqueue/epoll close 后跨轮 stale event 读已释放内存触发 UAF
void _uev_qtn_push(watcher_ctx *watcher, void *obj, qtn_type type);
// 减 listener_ctx 引用计数，归 0 时入 watcher->qtn（替代 _defer_freelsn 旧路径）
// 封装让外部模块（如 cmds.c）不直接触碰 listener_ctx 内部字段
void _uev_qtn_freelsn(watcher_ctx *watcher, struct listener_ctx *lsn);
// _uev_loop_event 末尾扫描隔离队列：队头超过 QTN_MS 则真释放（FIFO 性质，
// 队头未到期则后续都未到期，O(1) 检查即可退出）
void _uev_qtn_drain(watcher_ctx *watcher, uint64_t now_ms);
// ev_free 时强制清空隔离队列：watcher 已停，所有对象立即真释放
void _uev_qtn_flush(watcher_ctx *watcher);
// 获取sock_ctx对应的ud_cxt指针
ud_cxt *_uev_get_ud(sock_ctx *skctx);
// 校验skid是否与当前连接匹配（防止fd复用误操作）
int32_t _uev_check_skid(sock_ctx *skctx, const uint64_t skid);

#endif//EV_IOCP
#endif//UEV_H_
