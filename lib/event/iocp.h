#ifndef IOCP_H_
#define IOCP_H_

#include "event/event.h"
#include "event/skpool.h"
#include "thread/thread.h"
#include "thread/spinlock.h"

#ifdef EV_IOCP

struct listener_ctx;
// IOCP 事件循环内部命令枚举
typedef enum WEV_CMDS {
    CMD_STOP = 0x00,  // 停止事件循环
    CMD_DISCONN,      // 断开连接
    CMD_ADD,          // 添加socket到hashmap
    CMD_ADDACP,       // 将accept到的fd加入事件循环
    CMD_REMOVE,       // 从hashmap移除socket
    CMD_SEND,         // 发送数据
    CMD_SETUD,        // 设置ud_cxt字段
    CMD_SSL,          // 切换为SSL连接

    CMD_TOTAL,        // 命令总数（用于数组大小）
}WEV_CMDS;
// IOCP事件回调函数类型
typedef void(*event_cb)(void *arg, struct sock_ctx *skctx, DWORD bytes);
// IOCP socket上下文基础结构（内嵌OVERLAPPED，供IOCP使用）
typedef struct sock_ctx {
    OVERLAPPED overlapped; // IOCP重叠结构，必须位于首字段
    int32_t type;          // socket类型（SOCK_STREAM/SOCK_DGRAM）
    SOCKET fd;             // socket句柄
    event_cb ev_cb;        // 事件触发时的回调函数
}sock_ctx;
// 命令上下文，通过sock_pair管道在线程间传递
typedef struct cmd_ctx {
    int32_t cmd;    // 命令类型 WEV_CMDS
    SOCKET fd;      // 目标socket句柄
    size_t len;     // 数据长度或附加参数
    uint64_t skid;  // 连接ID（防止fd复用导致的误操作）
    uint64_t arg;   // 命令携带的指针或值参数
}cmd_ctx;
QUEUE_DECL(cmd_ctx, qu_cmd);
// IOCP命令通道上下文（每个watcher拥有多个，用于接收跨线程命令）
typedef struct overlap_cmd_ctx {
    sock_ctx ol_r;      // 读端sock_ctx（接收触发信号）
    DWORD bytes;        // WSARecv接收到的字节数
    DWORD flag;         // WSARecv标志
    SOCKET fd;          // 写端socket（发送信号）
    qu_cmd_ctx qu;      // 命令队列
    spin_ctx spin;      // 保护qu的自旋锁
    IOV_TYPE wsabuf;    // WSARecv缓冲区
}overlap_cmd_ctx;
// 事件监听器上下文（每个工作线程一个）
typedef struct watcher_ctx {
    int32_t index;              // 当前watcher编号
    int32_t stop;               // 停止标志
    uint32_t ncmd;              // 命令通道数量
    HANDLE iocp;                // IOCP句柄
    ev_ctx *ev;                 // 所属ev_ctx
    overlap_cmd_ctx *cmd;       // 命令通道数组
    struct hashmap *element;    // fd -> sock_ctx 哈希表
    pthread_t thevent;          // 事件循环线程
    skpool_ctx pool;            // sock_ctx对象池
}watcher_ctx;
// AcceptEx专用线程上下文
typedef struct acceptex_ctx {
    int32_t index;  // 编号
    int32_t stop;   // 停止标志
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

// 向指定命令通道发送命令
void _send_cmd(watcher_ctx *watcher, uint32_t index, cmd_ctx *cmd);
// 发送CMD_ADD命令，将sock_ctx加入watcher的hashmap
void _cmd_add(watcher_ctx *watcher, sock_ctx *skctx, SOCKET fd);
// 发送CMD_ADDACP命令，通知watcher处理新accept的fd
void _cmd_add_acpfd(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn);
// 发送CMD_REMOVE命令，从watcher中移除fd
void _cmd_remove(watcher_ctx *watcher, SOCKET fd, uint64_t skid);

// 命令处理：停止事件循环
void _on_cmd_stop(watcher_ctx *watcher, cmd_ctx *cmd);
// 命令处理：将sock_ctx加入hashmap
void _on_cmd_add(watcher_ctx *watcher, cmd_ctx *cmd);
// 命令处理：将新accept的fd加入IOCP并初始化接收
void _on_cmd_addacp(watcher_ctx *watcher, cmd_ctx *cmd);
// 命令处理：从hashmap移除fd并回收到对象池
void _on_cmd_remove(watcher_ctx *watcher, cmd_ctx *cmd);
// 命令处理：触发SSL握手流程
void _on_cmd_ssl(watcher_ctx *watcher, cmd_ctx *cmd);
// 命令处理：将数据加入发送队列并触发发送
void _on_cmd_send(watcher_ctx *watcher, cmd_ctx *cmd);
// 命令处理：断开连接
void _on_cmd_disconn(watcher_ctx *watcher, cmd_ctx *cmd);
// 命令处理：设置ud_cxt字段值
void _on_cmd_setud(watcher_ctx *watcher, cmd_ctx *cmd);

// 将sock_ctx加入watcher的hashmap（断言不重复）
void _add_fd(watcher_ctx *watcher, sock_ctx *skctx);
// 从watcher的hashmap中移除fd
void _remove_fd(watcher_ctx *watcher, SOCKET fd);
// 将fd关联到IOCP句柄
int32_t _join_iocp(watcher_ctx *watcher, SOCKET fd);
// 尝试对已有连接启动SSL握手（支持延迟到发送完毕）
void _try_ssl_exchange(watcher_ctx *watcher, sock_ctx *skctx, struct evssl_ctx *evssl, int32_t client);
// 在事件循环内将accept到的fd完成初始化并开始接收
void _add_acpfd_inloop(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn);
// 提交WSARecv异步接收请求
int32_t _post_recv(sock_ctx *skctx, DWORD  *bytes, DWORD  *flag, IOV_TYPE *wsabuf, DWORD niov);
// 将数据加入TCP发送队列，若当前未发送则立即提交WSASend
void _add_bufs_trypost(sock_ctx *skctx, off_buf_ctx *buf);
// 将UDP数据加入发送队列，若当前未发送则立即提交WSASendTo
void _add_bufs_trysendto(sock_ctx *skctx, off_buf_ctx *buf);
// shutdown socket读端（触发对端关闭流程）
void _sk_shutdown(sock_ctx *skctx);
// 标记连接为错误状态并取消所有IOCP挂起操作
void _disconnect(sock_ctx *skctx);
// 释放UDP socket上下文
void _free_udp(sock_ctx *skctx);
// 释放listener_ctx
void _freelsn(struct listener_ctx *lsn);
// 获取sock_ctx对应的ud_cxt指针
ud_cxt *_get_ud(sock_ctx *skctx);
// 校验skid是否与当前连接匹配（防止fd复用误操作）
int32_t _check_skid(sock_ctx *skctx, const uint64_t skid);

#endif//EV_IOCP
#endif//IOCP_H_
