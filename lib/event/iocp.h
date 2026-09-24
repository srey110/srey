#ifndef IOCP_H_
#define IOCP_H_

#include "event/event.h"
#include "utils/pool.h"
#include "containers/hashmap.h"
#include "event/cmds.h"
#include "thread/thread.h"
#include "utils/tda.h"
#include "utils/timer.h"

#ifdef EV_IOCP

// IOCP事件回调函数类型
typedef void(*event_cb)(void *arg, struct evsock_ctx *evsk, DWORD bytes);
// IOCP socket上下文基础结构（内嵌OVERLAPPED，供IOCP使用）
typedef struct evsock_ctx {
    OVERLAPPED overlapped; // IOCP重叠结构，必须位于首字段
    int32_t type;          // socket类型（SOCK_STREAM/SOCK_DGRAM）
    event_cb ev_cb;        // 事件触发时的回调函数
    sock_ctx sk;           // 连接标识；listener/pipe 的 skid 恒为 0，createid() 最小返 1 故不与真连接碰撞
}evsock_ctx;
HASHMAP_DECL(sockel_map, evsock_ctx *, SOCKEL_HASH, SOCKEL_CMP)
// IOCP命令通道上下文（每个watcher一个；唤醒走 PQCS，ol_r 只是完成包回投的身份标记，不做真实 I/O）
typedef struct overlap_cmd_ctx {
    evsock_ctx ol_r;          // 完成包回投的 evsock_ctx：ev_cb = _iocp_on_cmd，fd 恒为 INVALID_SOCK
    tda_ctx tda;            // 队列长度告警翻倍状态（init = fsqu 容量 / QUEUE_OVERLOAD_RATIO）
    cmdq qu;            // 命令队列（多生产者，单消费者批量 pop；元素 cmd_ctx）
    atomic_t wake_pending;  // 唤醒在途标志：1=已投未消费，生产者据此不再重投；消费端抽队列前清 0
}overlap_cmd_ctx;
// 事件监听器上下文（每个工作线程一个）
typedef struct watcher_ctx {
    int32_t index;              // 当前watcher编号
    atomic_t stop;              // 停止标志
    HANDLE iocp;                // IOCP句柄
    ev_ctx *ev;                 // 所属ev_ctx
    sockel_map *element;        // fd -> evsock_ctx 哈希表
    pthread_t thevent;          // 事件循环线程
    pool_ctx pool;              // evsock_ctx对象池
    timer_ctx timer;            // 计时器
    overlap_cmd_ctx cmd;        // 命令通道（fsqu 多生产者，单通道足够）
    list_ctx ticks;             // event 线程周期驱动节点(ev_tick)链表
    list_ctx lingers;           // 延迟关闭中的连接(按进入时刻先后串,队头最旧)
    ev_tick linger_tick;        // 驱动上面那条链的 tick(cb 非 NULL 表示已挂;链空即摘)
#if WITH_SSL
    list_ctx wpends;            // 挂起 SSL 写的连接(按 wpend_ms 先后串,队头最旧)
    ev_tick wpend_tick;         // 驱动上面那条链的 tick(cb 非 NULL 表示已挂;链空即摘)
#endif
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
// 对 watcher 下每个存活 socket 发起立即 disconnect(CancelIoEx)，CMD_STOP 时调用
void _iocp_disconnect_all(watcher_ctx *watcher);
// 尝试对已有连接启动SSL握手（支持延迟到发送完毕）
void _iocp_try_ssl_exchange(watcher_ctx *watcher, evsock_ctx *evsk, struct evssl_ctx *evssl, int32_t client);
// 对已握手的 TLS1.3 连接排程一次 KeyUpdate，口径同 uev.h 的 _uev_keyupdate
void _iocp_keyupdate(watcher_ctx *watcher, evsock_ctx *evsk, int32_t updatetype);
// 在事件循环内将accept到的fd完成初始化并开始接收
void _iocp_add_acpfd_inloop(watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn);
// 在watcher线程内注册连接中的socket：sockel_add后投递ConnectEx
void _iocp_add_conn_inloop(watcher_ctx *watcher, struct evsock_ctx *evsk, netaddr_ctx *addr);
// 在watcher线程内注册socket：sockel_add后按TCP/UDP投递WSARecv/WSARecvFrom
void _iocp_add_fd_inloop(watcher_ctx *watcher, struct evsock_ctx *evsk);
// 提交WSARecv异步接收请求
int32_t _iocp_post_recv(evsock_ctx *evsk, DWORD *bytes, DWORD *flag, IOV_TYPE *wsabuf, DWORD niov);
// 将数据加入TCP发送队列，若当前未发送则立即提交WSASend
void _iocp_add_bufs_trypost(evsock_ctx *evsk, off_buf_ctx *buf);
// 队列空且无在途 IRP 时同步 WSASendTo 一次，省掉调用方的 MALLOC+memcpy；返回值契约同 _evpub_try_sendto，
// 但同步失败一律返 1 让调用方排队，不像 uev 侧那样就地断连
int32_t _iocp_try_sendto(evsock_ctx *evsk, const void *data, size_t len, netaddr_ctx *addr);
// 将UDP数据加入发送队列，若当前未发送则立即提交WSASendTo
void _iocp_add_bufs_trysendto(watcher_ctx *watcher, evsock_ctx *evsk, sendto_ctx *buf);
// 关闭流程里的 shutdown：how 取 SHUT_RD 或 SHUT_WR，有 SSL 先发 close_notify
void _iocp_sk_shutdown(evsock_ctx *evsk, int32_t how);
// 标记连接为错误状态并取消所有IOCP挂起操作
// TCP 先走 _evpub_close_flush_tcp 冲一次 send queue，再 CancelIoEx 掉在途探针；其余同 _uev_disconnect
void _iocp_disconnect(evsock_ctx *evsk);
// 释放UDP socket上下文
void _iocp_free_udp(evsock_ctx *evsk);
// 释放listener_ctx
void _iocp_freelsn(struct listener_ctx *lsn);
// 递减 listener_ctx 引用计数，归零后释放（跨文件路径访问 lsn->ref 的封装）
void _iocp_try_freelsn(struct listener_ctx *lsn);
// ev_free 关闭阶段 ev_unlisten 掉所有残留 listener（走完成驱动释放，非强制 FREE）
void _iocp_unlisten_all(ev_ctx *ctx);
// acpex 循环周期性调用：复活 dead 的 AcceptEx 槽（覆盖全槽 dead 无 accept 完成的死锁）
void _olp_revive_dead(ev_ctx *ev);
// ev_free 排空阶段直接释放一个 AcceptEx 完成对应的 listener 引用（跳过 _olp_on_accept_cb 分支）
void _iocp_acpex_release(evsock_ctx *evsk);
// 获取evsock_ctx对应的ud_cxt指针
ud_cxt *_iocp_get_ud(evsock_ctx *evsk);
// 校验skid是否与当前连接匹配（防止fd复用误操作）
int32_t _iocp_check_skid(evsock_ctx *evsk, const uint64_t skid);

#endif//EV_IOCP
#endif//IOCP_H_
