#ifndef IOCP_H_
#define IOCP_H_

#include "event/event.h"
#include "utils/pool.h"
#include "event/cmds.h"
#include "thread/thread.h"
#include "utils/tda.h"

#ifdef EV_IOCP

// IOCP事件回调函数类型
typedef void(*event_cb)(void *arg, struct sock_ctx *skctx, DWORD bytes);
// IOCP socket上下文基础结构（内嵌OVERLAPPED，供IOCP使用）
typedef struct sock_ctx {
    OVERLAPPED overlapped; // IOCP重叠结构，必须位于首字段
    int32_t type;          // socket类型（SOCK_STREAM/SOCK_DGRAM）
    SOCKET fd;             // socket句柄
    event_cb ev_cb;        // 事件触发时的回调函数
}sock_ctx;
// IOCP命令通道上下文（每个watcher拥有多个，用于接收跨线程命令）
typedef struct overlap_cmd_ctx {
    sock_ctx ol_r;          // 读端sock_ctx（接收触发信号）
    DWORD bytes;            // WSARecv接收到的字节数
    DWORD flag;             // WSARecv标志
    SOCKET fd;              // 写端socket（发送信号）
    tda_ctx tda;                // 队列长度告警翻倍状态（init = fsqu 容量 / QUEUE_OVERLOAD_RATIO）
    fsqu_ctx qu;                // 命令队列（多生产者，单消费者批量 pop；元素 cmd_ctx）
    IOV_TYPE wsabuf;        // WSARecv缓冲区
}overlap_cmd_ctx;
// 事件监听器上下文（每个工作线程一个）
typedef struct watcher_ctx {
    int32_t index;              // 当前watcher编号
    atomic_t stop;              // 停止标志
    HANDLE iocp;                // IOCP句柄
    ev_ctx *ev;                 // 所属ev_ctx
    struct hashmap *element;    // fd -> sock_ctx 哈希表
    pthread_t thevent;          // 事件循环线程
    pool_ctx pool;              // sock_ctx对象池
    overlap_cmd_ctx cmd;        // 命令通道（fsqu 多生产者，单通道足够）
}watcher_ctx;
// AcceptEx专用线程上下文
typedef struct acceptex_ctx {
    int32_t index;  // 编号
    atomic_t stop;  // 停止标志
    ev_ctx *ev;     // 所属ev_ctx
    HANDLE iocp;    // AcceptEx专用IOCP句柄
    pthread_t thacp; // AcceptEx线程
}acceptex_ctx;
// Windows扩展函数指针集合（AcceptEx/ConnectEx）
typedef struct exfuncs_ctx {
    BOOL(WINAPI *acceptex)(SOCKET, SOCKET, PVOID, DWORD, DWORD, DWORD, LPDWORD, LPOVERLAPPED);  // AcceptEx函数指针
    BOOL(WINAPI *connectex)(SOCKET, const struct sockaddr *, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED); // ConnectEx函数指针
}exfuncs_ctx;
extern exfuncs_ctx _exfuncs; // 全局扩展函数指针（懒加载初始化）

// 将fd关联到IOCP句柄
int32_t _iocp_join(watcher_ctx *watcher, SOCKET fd);
// 尝试对已有连接启动SSL握手（支持延迟到发送完毕）
void _iocp_try_ssl_exchange(watcher_ctx *watcher, sock_ctx *skctx, struct evssl_ctx *evssl, int32_t client);
// 在事件循环内将accept到的fd完成初始化并开始接收
void _iocp_add_acpfd_inloop(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn);
// 在watcher线程内注册连接中的socket：sockel_add后投递ConnectEx
void _iocp_add_conn_inloop(watcher_ctx *watcher, struct sock_ctx *skctx, netaddr_ctx *addr);
// 在watcher线程内注册socket：sockel_add后按TCP/UDP投递WSARecv/WSARecvFrom
void _iocp_add_fd_inloop(watcher_ctx *watcher, struct sock_ctx *skctx);
// 提交WSARecv异步接收请求
int32_t _iocp_post_recv(sock_ctx *skctx, DWORD *bytes, DWORD *flag, IOV_TYPE *wsabuf, DWORD niov);
// 将数据加入TCP发送队列，若当前未发送则立即提交WSASend
void _iocp_add_bufs_trypost(sock_ctx *skctx, off_buf_ctx *buf);
// 将UDP数据加入发送队列，若当前未发送则立即提交WSASendTo
void _iocp_add_bufs_trysendto(sock_ctx *skctx, sendto_ctx *buf);
// shutdown socket读端（触发对端关闭流程）
void _iocp_sk_shutdown(sock_ctx *skctx);
// 标记连接为错误状态并取消所有IOCP挂起操作
// immed=1 立即关(STATUS_ERROR + CancelIoEx 取消 in-flight,丢弃未发数据)
// immed=0 优雅关(STATUS_GRACEFUL_CLOSE,outstanding WSASend 完成后 _on_send_cb 触发 _close_tcp;
//                buf_s empty 且无 SENDING 时退化为立即关)
void _iocp_disconnect(sock_ctx *skctx, int32_t immed);
// 释放UDP socket上下文
void _iocp_free_udp(sock_ctx *skctx);
// 释放listener_ctx
void _iocp_freelsn(struct listener_ctx *lsn);
// 递减 listener_ctx 引用计数，归零后释放（跨文件路径访问 lsn->ref 的封装）
void _iocp_try_freelsn(struct listener_ctx *lsn);
// 获取sock_ctx对应的ud_cxt指针
ud_cxt *_iocp_get_ud(sock_ctx *skctx);
// 校验skid是否与当前连接匹配（防止fd复用误操作）
int32_t _iocp_check_skid(sock_ctx *skctx, const uint64_t skid);

#endif//EV_IOCP
#endif//IOCP_H_
