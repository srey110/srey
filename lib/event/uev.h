#ifndef UEV_H_
#define UEV_H_

#include "event/event.h"
#include "event/skpool.h"
#include "thread/thread.h"

#ifndef EV_IOCP

// 根据平台选择对应的事件结构体类型
#if defined(EV_EPOLL)
typedef struct epoll_event events_t;
#elif defined(EV_KQUEUE)
typedef struct kevent events_t;
typedef struct kevent changes_t;
#define COMMIT_NCHANGES   // kqueue/devpoll：需批量提交变更列表
#elif defined(EV_EVPORT)
typedef port_event_t events_t;
#define MANUAL_ADD        // evport：每次触发后需手动重新注册
#elif defined(EV_POLLSET)
typedef struct pollfd events_t;
#define MANUAL_REMOVE     // pollset：关闭时需手动从pollset中删除
#define NO_UDATA          // pollset/devpoll：事件不携带用户数据，需从hashmap查找
#elif defined(EV_DEVPOLL)
typedef struct pollfd events_t;
typedef struct pollfd changes_t;
#define MANUAL_REMOVE
#define COMMIT_NCHANGES
#define NO_UDATA
#endif

struct conn_ctx;
struct listener_ctx;
// I/O事件类型
typedef enum EVENTS {
    EVENT_READ = 0x01,  // 可读事件
    EVENT_WRITE = 0x02, // 可写事件
}EVENTS;
// 事件循环内部命令枚举（Unix版本）
typedef enum UEV_CMDS {
    CMD_STOP = 0x00,  // 停止事件循环
    CMD_DISCONN,      // 断开连接
    CMD_LSN,          // 添加监听socket
    CMD_UNLSN,        // 取消监听
    CMD_CONN,         // 添加连接中的socket
    CMD_ADDACP,       // 将accept到的fd加入事件循环
    CMD_ADDUDP,       // 添加UDP socket
    CMD_SEND,         // 发送数据
    CMD_SETUD,        // 设置ud_cxt字段
    CMD_SSL,          // 切换为SSL连接

    CMD_TOTAL,        // 命令总数（用于数组大小）
}UEV_CMDS;
// 命令上下文，通过匿名管道在线程间传递
typedef struct cmd_ctx {
    int32_t cmd;    // 命令类型 UEV_CMDS
    SOCKET fd;      // 目标socket句柄
    size_t len;     // 数据长度或附加参数
    uint64_t skid;  // 连接ID（防止fd复用导致的误操作）
    uint64_t arg;   // 命令携带的指针或值参数
}cmd_ctx;
// 事件监听器上下文（每个工作线程一个）
typedef struct watcher_ctx {
    int32_t index;              // 当前watcher编号
    int32_t stop;               // 停止标志
    int32_t evfd;               // epoll/kqueue/evport等的事件fd
    int32_t nevents;            // events数组容量
    uint32_t npipes;            // 命令管道数量
#ifdef COMMIT_NCHANGES
    int32_t nsize;              // changes数组容量
    int32_t nchanges;           // 待提交的变更数量
    changes_t *changes;         // 变更列表（kqueue/devpoll使用）
#endif
    events_t *events;           // 就绪事件数组
    struct pip_ctx *pipes;      // 命令管道数组
    ev_ctx *ev;                 // 所属ev_ctx
    struct hashmap *element;    // fd -> sock_ctx 哈希表
    pthread_t thevent;          // 事件循环线程
    skpool_ctx pool;            // sock_ctx对象池
}watcher_ctx;

// 事件回调函数类型
typedef void(*event_cb)(watcher_ctx *watcher, struct sock_ctx *skctx, int32_t ev);
// Unix平台的socket上下文基础结构
typedef struct sock_ctx {
    SOCKET fd;          // socket句柄
    int32_t type;       // socket类型（SOCK_STREAM/SOCK_DGRAM/0表示pipe/listen）
    int32_t events;     // 当前注册的事件掩码
    event_cb ev_cb;     // 事件触发时的回调函数
}sock_ctx;

// 向事件多路复用器注册或追加监听事件
int32_t _add_event(watcher_ctx *watcher, SOCKET fd, int32_t *events, int32_t ev, void *arg);
// 从事件多路复用器删除或减少监听事件
void _del_event(watcher_ctx *watcher, SOCKET fd, int32_t *events, int32_t ev, void *arg);

// 向指定管道发送命令（阻塞直到成功或watcher停止）
void _send_cmd(watcher_ctx *watcher, uint32_t index, cmd_ctx *cmd);
// 发送CMD_CONN命令，将连接中的sock_ctx交给对应watcher处理
void _cmd_connect(ev_ctx *ctx, SOCKET fd, sock_ctx *skctx);
// 发送CMD_LSN命令，通知watcher注册监听socket
void _cmd_listen(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx);
// 发送CMD_UNLSN命令，通知watcher取消监听
void _cmd_unlisten(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn);
// 发送CMD_ADDACP命令，通知watcher处理新accept的fd
void _cmd_add_acpfd(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn);
// 发送CMD_ADDUDP命令，通知watcher注册UDP socket
void _cmd_add_udp(ev_ctx *ctx, SOCKET fd, sock_ctx *skctx);

// 命令处理：停止事件循环
void _on_cmd_stop(watcher_ctx *watcher, cmd_ctx *cmd);
// 命令处理：断开连接
void _on_cmd_disconn(watcher_ctx *watcher, cmd_ctx *cmd);
// 命令处理：在事件循环内完成监听注册
void _on_cmd_lsn(watcher_ctx *watcher, cmd_ctx *cmd);
// 命令处理：在事件循环内取消监听
void _on_cmd_unlsn(watcher_ctx *watcher, cmd_ctx *cmd);
// 命令处理：在事件循环内注册连接中的socket
void _on_cmd_conn(watcher_ctx *watcher, cmd_ctx *cmd);
// 命令处理：触发SSL握手流程
void _on_cmd_ssl(watcher_ctx *watcher, cmd_ctx *cmd);
// 命令处理：将数据加入发送队列并触发写事件
void _on_cmd_send(watcher_ctx *watcher, cmd_ctx *cmd);
// 命令处理：在事件循环内完成accept fd的初始化
void _on_cmd_addacp(watcher_ctx *watcher, cmd_ctx *cmd);
// 命令处理：在事件循环内注册UDP socket
void _on_cmd_add_udp(watcher_ctx *watcher, cmd_ctx *cmd);
// 命令处理：设置ud_cxt字段值
void _on_cmd_setud(watcher_ctx *watcher, cmd_ctx *cmd);

// 在事件循环内完成监听socket的注册
void _add_lsn_inloop(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx);
// 在事件循环内取消监听，引用计数归零后释放listener_ctx
void _remove_lsn(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn);
// 尝试对已有连接启动SSL握手（支持延迟到发送完毕）
void _try_ssl_exchange(watcher_ctx *watcher, sock_ctx *skctx, struct evssl_ctx *evssl, int32_t client);
// 在事件循环内将连接中的fd注册可写事件（等待connect完成）
void _add_conn_inloop(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx);
// 在事件循环内将accept到的fd完成初始化并注册读事件
void _add_acpfd_inloop(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn);
// 将数据加入发送队列，并确保注册写事件
void _add_write_inloop(watcher_ctx *watcher, sock_ctx *skctx, off_buf_ctx *buf);
// 在事件循环内将UDP socket注册读事件
void _add_udp_inloop(watcher_ctx *watcher, SOCKET fd, sock_ctx *skctx);

// 将sock_ctx加入watcher的hashmap（断言不重复）
void _add_fd(watcher_ctx *watcher, sock_ctx *skctx);
// 根据fd从hashmap查找sock_ctx
sock_ctx *_map_get(watcher_ctx *watcher, SOCKET fd);
// shutdown socket读端（触发关闭流程）
void _sk_shutdown(sock_ctx *skctx);
// 释放UDP socket上下文
void _free_udp(sock_ctx *skctx);
// 标记连接为错误状态并触发关闭（TCP shutdown/UDP注册写事件）
void _disconnect(watcher_ctx *watcher, sock_ctx *skctx);
// 释放listener_ctx
void _freelsn(struct listener_ctx *lsn);
// 获取sock_ctx对应的ud_cxt指针
ud_cxt *_get_ud(sock_ctx *skctx);
// 校验skid是否与当前连接匹配（防止fd复用误操作）
int32_t _check_skid(sock_ctx *skctx, const uint64_t skid);

#endif//EV_IOCP
#endif//UEV_H_
