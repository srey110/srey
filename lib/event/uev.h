#ifndef UEV_H_
#define UEV_H_

#include "event/event.h"
#include "utils/pool.h"
#include "containers/hashmap.h"
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
    // 命令唤醒改用 kqueue 的用户事件:不建管道,消费端也省掉那次清可读态的 read
    // (口径同 IOCP 侧的 PQCS)。注释掉即整条退回管道,排查时可用。
    // OpenBSD 归在 OS_BSD 却没有 EVFILT_USER,靠这层守卫自动让开,不必手动关
    #ifdef EVFILT_USER
        #define NO_CMD_PIPE
    #endif
    // 攒发水位:本轮攒下的待发字节超此数就先冲一批,不定义即不攒(flush_bytes 一并去掉)。
    // 攒得多能合并 writev,但载荷要等到冲的那一刻才被内核读走,堆太多反而慢。
    // 只有 kqueue 受益,epoll 上反而退化,故只在 kqueue 下定义
    #define FLUSH_WATERMARK (64 * 1024)
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
typedef enum events {
    EVENT_READ = 0x01,  // 可读事件
    EVENT_WRITE = 0x02, // 可写事件
#if defined(EV_KQUEUE)
    EVENT_ERROR = 0x04, // 事件注册失败；仅 kqueue 需要（其余平台注册失败由 _uev_add_event 同步返回）
#endif
}events;
// 隔离队列元素类型
typedef enum qtn_type {
    QTN_TCP,    // 出队 → pool_push
    QTN_UDP,    // 出队 → _uev_free_udp
    QTN_LSN,    // 出队 → _uev_freelsn
}qtn_type;
// 事件回调函数类型
typedef void(*event_cb)(struct watcher_ctx *watcher, struct evsock_ctx *evsk, int32_t ev);
// Unix平台的socket上下文基础结构
typedef struct evsock_ctx {
    int32_t type;       // socket类型（SOCK_STREAM/SOCK_DGRAM/0表示pipe/listen）
    int32_t events;     // 当前注册的事件掩码
#ifdef COMMIT_NCHANGES
    uint32_t chg_round; // 最后一次往 changes 追加条目时的轮次，用途见 _uev_drop_changes
#endif
    event_cb ev_cb;     // 事件触发时的回调函数
    sock_ctx sk;        // 连接标识；listener/pipe 的 skid 恒为 0，createid() 最小返 1 故不与真连接碰撞
}evsock_ctx;
HASHMAP_DECL(sockel_map, evsock_ctx *, SOCKEL_HASH, SOCKEL_CMP)
// 命令通道上下文：命令存 fsqu，唤醒信号走管道（NO_CMD_PIPE 下走 kqueue 用户事件，不建管道）
typedef struct pip_ctx {
#ifndef NO_CMD_PIPE
    int32_t pipes[2];               // pipes[0] 读端，pipes[1] 写端
#endif
    tda_ctx tda;                    // 队列长度告警翻倍状态（init = fsqu 容量 / QUEUE_OVERLOAD_RATIO）
    evsock_ctx skpip;                 // 唤醒信号的evsock_ctx（ev_cb = _uev_cmd_loop）；NO_CMD_PIPE 下 fd 恒 INVALID_SOCK
    cmdq qu;
    atomic_t wake_pending;          // 同 overlap_cmd_ctx.wake_pending
}pip_ctx;
// 隔离队列元素：close 后对象先入此队列暂存 QTN_MS 毫秒，让 stale event 消化完再真释放
typedef struct qtn_entry {
    qtn_type type;
    void *obj;          // evsock_ctx * 或 listener_ctx *
    uint64_t enter_ms;  // 入队时刻（monotonic 毫秒）
}qtn_entry;
QUE_DECL(qtn_que, qtn_entry)// 隔离队列
// 事件监听器上下文（每个工作线程一个）
typedef struct watcher_ctx {
    int32_t index;              // 当前watcher编号
    atomic_t stop;              // 停止标志
    int32_t evfd;               // epoll/kqueue/evport等的事件fd
    int32_t nevents;            // events数组容量
#ifdef COMMIT_NCHANGES
    int32_t nsize;              // changes数组容量
    int32_t nchanges;           // 待提交的变更数量
    uint32_t chg_round;         // 事件循环轮次，每轮 +1；与 evsock_ctx.chg_round 配对，见 _uev_drop_changes
    changes_t *changes;         // 变更列表（kqueue/devpoll使用）
#endif
    events_t *events;           // 就绪事件数组
    ev_ctx *ev;                 // 所属ev_ctx
    sockel_map *element;        // fd -> evsock_ctx 哈希表
    pthread_t thevent;          // 事件循环线程
    pool_ctx pool;              // evsock_ctx对象池
    timer_ctx timer;            // 计时器
    qtn_que qtn;                // 隔离队列 FIFO
    pip_ctx pipe;               // 命令通道（fsqu 存命令 + 单管道传唤醒信号）
    list_ctx ticks;             // event 线程周期驱动节点(ev_tick)链表
#ifdef FLUSH_WATERMARK
    size_t flush_bytes;         // 本轮攒下的待发字节合计，超水位就地冲；由 _uev_flush_pending 清零
#endif
    list_ctx flushes;           // 本轮攒下待发的连接(STATUS_FLUSHPEND 置位期间在链上)，
                                // 由 _uev_loop_event 每轮派发后统一冲；见 _usk_flush_link
    list_ctx lingers;           // 延迟关闭中的连接(按进入时刻先后串,队头最旧)
    ev_tick linger_tick;        // 驱动上面那条链的 tick(cb 非 NULL 表示已挂;链空即摘)
#if WITH_SSL
    list_ctx wpends;            // 挂起 SSL 写的连接(按 wpend_ms 先后串,队头最旧)
    ev_tick wpend_tick;         // 驱动上面那条链的 tick(cb 非 NULL 表示已挂;链空即摘)
#endif
    char udp_rbuf[MAX_RECVFROM_SIZE]; // UDP 接收共享缓冲：本线程所有 UDP socket 复用
}watcher_ctx;

// 向事件多路复用器注册或追加监听事件；批量提交的平台上真排了条目时给 evsk 打轮次戳
int32_t _uev_add_event(watcher_ctx *watcher, SOCKET fd, int32_t *curevents, int32_t ev, evsock_ctx *evsk);
// 从事件多路复用器删除或减少监听事件；打戳同 _uev_add_event
void _uev_del_event(watcher_ctx *watcher, SOCKET fd, int32_t *curevents, int32_t ev, evsock_ctx *evsk);
// close fd 前从待提交 changes 移除该 fd 的项，防 fd 复用后陈旧变更(旧 udata)落到新 fd；非 kqueue/devpoll 平台空操作
void _uev_drop_changes(watcher_ctx *watcher, evsock_ctx *evsk);
// 在事件循环内完成监听socket的注册
void _uev_add_lsn_inloop(watcher_ctx *watcher, evsock_ctx *evsk);
// 在事件循环内取消监听，引用计数归零后释放listener_ctx
void _uev_remove_lsn(watcher_ctx *watcher, struct listener_ctx *lsn);
// 尝试对已有连接启动SSL握手（支持延迟到发送完毕）
void _uev_try_ssl_exchange(watcher_ctx *watcher, evsock_ctx *evsk, struct evssl_ctx *evssl, int32_t client);
// 对已握手的 TLS1.3 连接排程一次 KeyUpdate，契约见 event.h 的 ev_keyupdate
void _uev_keyupdate(watcher_ctx *watcher, evsock_ctx *evsk, int32_t updatetype);
// 在事件循环内将连接中的fd注册可写事件（等待connect完成）
void _uev_add_conn_inloop(watcher_ctx *watcher, evsock_ctx *evsk);
// 在事件循环内将accept到的fd完成初始化并注册读事件
void _uev_add_acpfd_inloop(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn);
// 将 TCP 数据加入发送队列；从空队列开始的那条挂进 watcher->flushes 攒着，不立即发
void _uev_add_bufs_send(watcher_ctx *watcher, evsock_ctx *evsk, off_buf_ctx *buf);
// 把 watcher->flushes 上攒的连接逐个发出去，每条一次 writev 合并本轮攒下的全部 buf。
// 由事件线程在整轮派发后调用：攒的意义就是让同一 fd 的多条 ev_send 合成一次 syscall
void _uev_flush_pending(watcher_ctx *watcher);
// 将 UDP datagram(sendto_ctx) 加入发送队列，并确保注册写事件；
// tried 非 0 表示调用方入队前已尝试过一次发送（如 _uev_try_sendto 遇到 EAGAIN），跳过重复尝试
void _uev_add_bufs_sendto(watcher_ctx *watcher, evsock_ctx *evsk, sendto_ctx *buf, int32_t tried);
// 尝试直接发送；返回 0 表示已处理完(发送成功或致命错误已断开)，调用方无需任何后续操作；
// 返回 1 表示需要调用方继续(发送队列已有积压或 EAGAIN)，自行以 tried=1 转入 _uev_add_bufs_sendto 排队
int32_t _uev_try_sendto(watcher_ctx *watcher, evsock_ctx *evsk, const void *data, size_t len, netaddr_ctx *addr);
// 在事件循环内将socket注册读事件（TCP/UDP通用）
void _uev_add_fd_inloop(watcher_ctx *watcher, evsock_ctx *evsk);

// 关闭流程里的 shutdown：how 取 SHUT_RD 或 SHUT_WR，有 SSL 先发 close_notify
void _uev_sk_shutdown(evsock_ctx *evsk, int32_t how);
// 释放UDP socket上下文
void _uev_free_udp(evsock_ctx *evsk);
// 标记连接为错误状态并触发关闭（TCP shutdown/UDP注册写事件）；已置 STATUS_ERROR 时直接返回
// TCP 关闭前先走 _evpub_close_flush_tcp 冲一次 send queue；UDP 无待发队列不冲
void _uev_disconnect(watcher_ctx *watcher, evsock_ctx *evsk);
// 释放listener_ctx（立即释放，用于主线程兜底 / worker 退出后 cleanup 路径）
void _uev_freelsn(struct listener_ctx *lsn);
// 递减 listener_ctx 引用计数，归零后立即释放
// 主线程 / drain 残留路径用 (无 _uev_loop_event 在迭代，立即 FREE 安全)
void _uev_try_freelsn(struct listener_ctx *lsn);
// 释放对象入隔离队列 watcher->qtn；QTN_MS 毫秒后由 _uev_qtn_drain 真正释放
// 用于避开 kqueue/epoll close 后跨轮 stale event 读已释放内存触发 UAF
void _uev_qtn_push(watcher_ctx *watcher, void *obj, qtn_type type);
// 减 listener_ctx 引用计数，归 0 时入 watcher->qtn
// 封装让外部模块（如 cmds.c）不直接触碰 listener_ctx 内部字段
void _uev_qtn_freelsn(watcher_ctx *watcher, struct listener_ctx *lsn);
// _uev_loop_event 末尾扫描隔离队列：队头超过 QTN_MS 则真释放（FIFO 性质，
// 队头未到期则后续都未到期，O(1) 检查即可退出）
void _uev_qtn_drain(watcher_ctx *watcher, uint64_t now_ms);
// ev_free 时强制清空隔离队列：watcher 已停，所有对象立即真释放
void _uev_qtn_flush(watcher_ctx *watcher);
// 获取evsock_ctx对应的ud_cxt指针
ud_cxt *_uev_get_ud(evsock_ctx *evsk);
// 校验skid是否与当前连接匹配（防止fd复用误操作）
int32_t _uev_check_skid(evsock_ctx *evsk, const uint64_t skid);

#endif//EV_IOCP
#endif//UEV_H_
