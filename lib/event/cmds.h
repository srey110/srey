#ifndef CMDS_H_
#define CMDS_H_

#include "event/event.h"

// 事件循环内部命令枚举
typedef enum EV_CMDS {
    CMD_STOP = 0x00,  // [ev_free → _on_cmd_stop] 停止事件循环
    CMD_DISCONN,      // [ev_close → _on_cmd_disconn] 断开连接
    CMD_ADDACP,       // [_cmd_add_acpfd → _on_cmd_addacp] 将accept到的fd加入事件循环
    CMD_CONN,         // [_cmd_connect → _on_cmd_conn] 添加连接中的socket
    CMD_ADD,          // [_cmd_add → _on_cmd_add] 添加socket
    CMD_SEND,         // [ev_send → _on_cmd_send] 发送TCP数据（裸 payload）
    CMD_SEND_MULTI,   // [ev_send_multi → _on_cmd_send_multi] 多播发送 TCP 数据：args.multi.pack = shared_data* 共享 pack 指针(N 个 fd 共享同一份 data,各 buf 释放时 ref--)
    CMD_SENDTO,       // [ev_sendto → _on_cmd_sendto] 发送UDP数据（buf 前缀 netaddr_ctx + payload，与 CMD_SEND 区分以校验 fd 类型）
    CMD_UDP_OPT,      // [ev_udp_* → _on_cmd_udp_opt] UDP 多播 setsockopt：args.udpop = udp_opt_arg* 参数包(JOIN/LEAVE/TTL/LOOP)
    CMD_SETUD,        // [ev_ud_* → _on_cmd_setud] 设置ud_cxt字段
    CMD_SSL,          // [ev_ssl → _on_cmd_ssl] 切换为SSL连接
#ifndef EV_IOCP
    CMD_LSN,          // [_cmd_listen → _on_cmd_lsn] 添加监听socket
    CMD_UNLSN,        // [_cmd_unlisten → _on_cmd_unlsn] 取消监听
    CMD_LSN_UNREF,    // [_cmd_lsn_unref → _on_cmd_lsn_unref] ev_unlisten 末尾减占位 ref
#endif

    CMD_TOTAL,        // 命令总数（用于数组大小）
}EV_CMDS;
// 命令上下文
typedef struct cmd_ctx {
    int32_t  cmd;    // 命令类型 EV_CMDS
    SOCKET   fd;     // 目标 socket（STOP/ADD/LSN/LSN_UNREF 不用）
    uint64_t skid;   // 连接ID（防 fd 复用误操作；DISCONN/SEND 系列/UDP_OPT/SSL/SETUD 用）
    union {
        int32_t immed;                // CMD_DISCONN：立即关闭标志
        struct sock_ctx *skctx;       // CMD_ADD / CMD_LSN：待加入事件循环的 socket
        struct listener_ctx *lsn;     // CMD_ADDACP / CMD_UNLSN / CMD_LSN_UNREF：监听对象
        struct udp_opt_arg *udpop;    // CMD_UDP_OPT：setsockopt 参数包
        struct { struct sock_ctx *skctx; netaddr_ctx addr; } conn; // CMD_CONN：连接中 socket + 目标地址(仅 IOCP 用)
        struct { size_t len; void *data; } send;         // CMD_SEND：长度 + 裸 payload
        struct { size_t len; shared_data *pack; } multi; // CMD_SEND_MULTI：长度 + 共享包
        struct { size_t len; void *data; } sendto;       // CMD_SENDTO：长度 + [netaddr_ctx + payload]
        struct { int32_t client; struct evssl_ctx *evssl; } ssl;   // CMD_SSL：是否客户端 + evssl
        struct { int32_t type; uint64_t val; } setud;    // CMD_SETUD：ud 字段类型 + 值
    } args;
}cmd_ctx;

// 向watcher投递命令。stop 非0失败,兜底释放只能由ev_free完成
int32_t _send_cmd(struct watcher_ctx *watcher, cmd_ctx *cmd);
// 发送CMD_ADDACP命令，通知watcher处理新accept的fd，stop 非0失败
int32_t _cmd_add_acpfd(struct watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn);
// CMD_ADDACP命令处理：完成 accept fd 的初始化
void _on_cmd_addacp(struct watcher_ctx *watcher, cmd_ctx *cmd);
// 发送CMD_CONN命令，将连接中的sock_ctx交给对应watcher处理，stop 非0失败
int32_t _cmd_connect(ev_ctx *ctx, struct sock_ctx *skctx, netaddr_ctx *addr);
// CMD_CONN命令处理：在事件循环内注册连接中的socket
void _on_cmd_conn(struct watcher_ctx *watcher, cmd_ctx *cmd);
// 发送CMD_ADD命令，stop 非0 会失败
int32_t _cmd_add(struct watcher_ctx *watcher, struct sock_ctx *skctx);
// CMD_ADD命令处理：添加 socket
void _on_cmd_add(struct watcher_ctx *watcher, cmd_ctx *cmd);
#ifndef EV_IOCP
// 发送CMD_LSN命令，通知watcher注册监听socket，stop 非0失败
int32_t _cmd_listen(struct watcher_ctx *watcher, struct sock_ctx *skctx);
// CMD_LSN命令处理：在事件循环内完成监听注册
void _on_cmd_lsn(struct watcher_ctx *watcher, cmd_ctx *cmd);
// 发送CMD_UNLSN命令，通知watcher取消监听，stop 非0失败
int32_t _cmd_unlisten(struct watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn);
// CMD_UNLSN命令处理：在事件循环内取消监听
void _on_cmd_unlsn(struct watcher_ctx *watcher, cmd_ctx *cmd);
// 发送CMD_LSN_UNREF命令，让 worker 在 _uev_cmd_loop 内减 lsn 占位 ref
// (ev_unlisten 末尾用, 让减占位在 worker 上下文走 qtn 隔离队列)，stop 非0失败
int32_t _cmd_lsn_unref(struct watcher_ctx *watcher, struct listener_ctx *lsn);
// CMD_LSN_UNREF命令处理：在事件循环内减 lsn 占位 ref (ev_unlisten 末尾发的减占位命令)
void _on_cmd_lsn_unref(struct watcher_ctx *watcher, cmd_ctx *cmd);
#endif //EV_IOCP
// ev_free CMD_STOP命令处理：停止事件循环
void _on_cmd_stop(struct watcher_ctx *watcher, cmd_ctx *cmd);
// ev_close CMD_DISCONN命令处理：断开连接
void _on_cmd_disconn(struct watcher_ctx *watcher, cmd_ctx *cmd);
// ev_ssl CMD_SSL命令处理：触发SSL握手流程
void _on_cmd_ssl(struct watcher_ctx *watcher, cmd_ctx *cmd);
// ev_send CMD_SEND命令处理：将TCP数据加入发送队列（校验 fd 类型为 SOCK_STREAM）
void _on_cmd_send(struct watcher_ctx *watcher, cmd_ctx *cmd);
// ev_send_multi CMD_SEND_MULTI命令处理：多播 TCP 数据,args.multi.pack = shared_data*；sock 失效时归还引用,有效时包装 off_buf(shared=pack) 入队
void _on_cmd_send_multi(struct watcher_ctx *watcher, cmd_ctx *cmd);
// ev_sendto CMD_SENDTO命令处理：将UDP数据加入发送队列（校验 fd 类型为 SOCK_DGRAM）
void _on_cmd_sendto(struct watcher_ctx *watcher, cmd_ctx *cmd);
// _send_udp_opt CMD_UDP_OPT命令处理：UDP 多播 setsockopt,args.udpop = udp_opt_arg*；按 sock family 分 IPv4/IPv6 分支调对应 IP_*/IPV6_* 选项,完成后 FREE(arg)
void _on_cmd_udp_opt(struct watcher_ctx *watcher, cmd_ctx *cmd);
// ev_ud_* CMD_SETUD命令处理：设置ud_cxt字段值
void _on_cmd_setud(struct watcher_ctx *watcher, cmd_ctx *cmd);

#endif//CMDS_H_
