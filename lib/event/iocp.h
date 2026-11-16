#ifndef IOCP_H_
#define IOCP_H_

#include "event/event.h"
#include "utils/pool.h"
#include "containers/hashmap.h"
#include "event/cmds.h"
#include "thread/thread.h"
#include "utils/tda.h"

#ifdef EV_IOCP

// \Device\Afd 轮询(IOCTL_AFD_POLL)：写不出去后等可写用，理由见 overlap.c 的 _olp_post_send_wait。
// 未公开接口，码值与结构体布局随系统内部定义，别自行改动
#define IOCTL_AFD_POLL 0x00012024
#define AFD_POLL_SEND 0x0004// 可写
#define AFD_POLL_ABORT 0x0010// 对端 RST
#define AFD_STATUS_PENDING 0x00000103L // NTSTATUS 的 STATUS_PENDING：已挂起、完成包随后到

// ntdll 的 NtCreateFile(开 \Device\Afd) 与 NtDeviceIoControlFile(投 AFD 轮询)
typedef LONG(NTAPI *_nt_create_file)(PHANDLE, ACCESS_MASK, void *, void *, PLARGE_INTEGER, ULONG, ULONG, ULONG,
                                     ULONG, PVOID, ULONG);
typedef LONG(NTAPI *_nt_ioctl_file)(HANDLE, HANDLE, PVOID, PVOID, PVOID, ULONG, PVOID, ULONG, PVOID, ULONG);
// AFD 轮询的单个句柄项
typedef struct afd_poll_handle {
    HANDLE handle;  // base socket
    ULONG events;   // 关心的 AFD_POLL_*；完成后改写为已发生的
    LONG status;    // 完成状态
}afd_poll_handle;
// AFD 轮询的参数兼结果，轮询在途期间内核要写它，完成前不能动
typedef struct afd_poll_info {
    LARGE_INTEGER timeout;      // 取最大值即不超时
    ULONG count;                // 句柄数，只能是 1
    ULONG exclusive;            // 恒 0
    afd_poll_handle handles[1];
}afd_poll_info;
// IOCP事件回调函数类型
typedef void(*event_cb)(void *arg, struct evsock_ctx *evsk, DWORD bytes);
// IOCP socket上下文基础结构（内嵌OVERLAPPED，供IOCP使用）
typedef struct evsock_ctx {
    OVERLAPPED overlapped; // IOCP重叠结构，必须位于首字段
    int32_t type;          // socket类型（SOCK_STREAM/SOCK_DGRAM）
    event_cb ev_cb;        // 事件触发时的回调函数
    sock_ctx sk;           // 连接标识；listener / 命令通道的 skid 恒为 0，createid() 最小返 1 故不与真连接碰撞
}evsock_ctx;
HASHMAP_DECL(sockel_map, evsock_ctx *, SOCKEL_HASH, SOCKEL_CMP)
// IOCP命令通道上下文（每个watcher一个；唤醒走 PQCS，ol_r 只是完成包回投的身份标记，不做真实 I/O）
typedef struct overlap_cmd_ctx {
    evsock_ctx ol_r;          // 完成包回投的 evsock_ctx：ev_cb = _iocp_on_cmd，fd 恒为 INVALID_SOCK
    tda_ctx tda;            // 队列长度告警翻倍状态（init = fsqu 容量 / QUEUE_OVERLOAD_RATIO）
    cmdq qu;            // 命令队列（多生产者，单消费者批量 pop；元素 cmd_ctx）
    atomic_t wake_pending;  // 唤醒标志：为 1 时生产者不投唤醒；何时清 0 见 _send_cmd
}overlap_cmd_ctx;
// 事件监听器上下文（每个工作线程一个）
typedef struct watcher_ctx {
    int32_t index;              // 当前watcher编号
    atomic_t stop;              // 停止标志
    HANDLE iocp;                // IOCP句柄
    HANDLE afd;                 // \Device\Afd 句柄，关联本 watcher 的 IOCP，等可写的轮询都投在它上面；打不开为 NULL
    ev_ctx *ev;                 // 所属ev_ctx
    sockel_map *element;        // fd -> evsock_ctx 哈希表
    pthread_t thevent;          // 事件循环线程
    pool_ctx pool;              // evsock_ctx对象池
    timer_ctx timer;            // 计时器
    overlap_cmd_ctx cmd;        // 命令通道（fsqu 多生产者，单通道足够）
    list_ctx ticks;             // event 线程周期驱动节点(ev_tick)链表
    defer_exec_que defer_execs; // ev_defer_exec 投来待跑的回调，与命令一起在进等待前排空
    list_ctx flushes;           // 本轮攒下待发的明文连接(STATUS_FLUSHPEND 置位期间在链上)，
                                // 由 _iocp_flush_pending 统一冲
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
// Windows扩展函数指针集合（AcceptEx/ConnectEx，以及 AFD 轮询用的两个 ntdll 函数）
typedef struct exfuncs_ctx {
    BOOL(WINAPI *acceptex)(SOCKET, SOCKET, PVOID, DWORD, DWORD, DWORD, LPDWORD, LPOVERLAPPED);  // AcceptEx函数指针
    BOOL(WINAPI *connectex)(SOCKET, const struct sockaddr *, int, PVOID, DWORD, LPDWORD, LPOVERLAPPED); // ConnectEx函数指针
    _nt_create_file ntcreatefile; // 取不到为 NULL，watcher 不开 AFD 句柄
    _nt_ioctl_file ntioctl;       // 同上
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
// 将数据加入TCP发送队列。未在发送时：挂进 watcher->flushes 攒着不立即发(已在链上且攒满
// MAX_SEND_NIOV 条就先同步发一批)，明文与 SSL 一样
void _iocp_add_bufs_trypost(watcher_ctx *watcher, evsock_ctx *evsk, off_buf_ctx *buf);
// 把 watcher->flushes 上攒的连接逐个同步发出，发不完的投 AFD 轮询等可写接力。
// 由事件线程在整轮派发后、以及进等待前抽完命令后调用
void _iocp_flush_pending(watcher_ctx *watcher);
// 队列空且无在途 IRP 时同步 WSASendTo 一次，省掉调用方的 MALLOC+memcpy；返回值契约同 _evpub_try_sendto，
// 但同步失败一律返 1 让调用方排队，不像 uev 侧那样就地断连
int32_t _iocp_try_sendto(evsock_ctx *evsk, const void *data, size_t len, netaddr_ctx *addr);
// 将UDP数据加入发送队列，若当前未发送则立即提交WSASendTo
void _iocp_add_bufs_trysendto(watcher_ctx *watcher, evsock_ctx *evsk, sendto_ctx *buf);
// 关闭流程里的 shutdown：how 取 SHUT_RD 或 SHUT_WR，有 SSL 先发 close_notify
void _iocp_sk_shutdown(evsock_ctx *evsk, int32_t how);
// 标记连接为错误状态并取消所有IOCP挂起操作
// TCP 先走 _evpub_close_flush_tcp 冲一次 send queue，再经 _iocp_cancel_io 取消在途 IO；其余同 _uev_disconnect
void _iocp_disconnect(evsock_ctx *evsk);
// 取消 socket 上的全部在途 IO；TCP 的 ol_s 上挂着 AFD 轮询时它投在 AFD 句柄上，要另外对那个句柄取消
void _iocp_cancel_io(evsock_ctx *evsk);
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
// 获取evsock_ctx对应的回调集合(TCP/UDP，口径同 _iocp_get_ud)
cbs_ctx *_iocp_get_cbs(evsock_ctx *evsk);
// 校验skid是否与当前连接匹配（防止fd复用误操作）
int32_t _iocp_check_skid(evsock_ctx *evsk, const uint64_t skid);

#endif//EV_IOCP
#endif//IOCP_H_
