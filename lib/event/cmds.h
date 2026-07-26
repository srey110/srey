#ifndef CMDS_H_
#define CMDS_H_

#include "event/event.h"

// 事件循环内部命令枚举
typedef enum ev_cmds {
    CMD_STOP = 0x00,  // [ev_free → _on_cmd_stop] 停止事件循环
    CMD_ADDACP,       // [_cmd_add_acpfd → _on_cmd_addacp] 将accept到的fd加入事件循环
    CMD_CONN,         // [_cmd_connect → _on_cmd_conn] 添加连接中的socket
    CMD_ADD,          // [_cmd_add → _on_cmd_add] 添加socket
    CMD_SENDTO,       // [ev_sendto → _on_cmd_sendto] 发送UDP数据：args.sendto = sendto_ctx(addr 与 payload 分离)，与 TCP 的 ev_send(CMD_PROPS) 区分以校验 fd 类型
#ifndef EV_IOCP
    CMD_LSN,          // [_cmd_listen → _on_cmd_lsn] 添加监听socket
    CMD_UNLSN,        // [_cmd_unlisten → _on_cmd_unlsn] 取消监听
    CMD_LSN_UNREF,    // [_cmd_lsn_unref → _on_cmd_lsn_unref] ev_unlisten 末尾减占位 ref
#endif
    CMD_PROPS,

    CMD_TOTAL        // 命令总数（用于数组大小）
}ev_cmds;
// 命令上下文
typedef struct cmd_ctx {
    int32_t cmd;// 命令类型 ev_cmds
    sk_id sk;// 目标连接 fd+skid（STOP/ADD/LSN/LSN_UNREF 不用 fd；skid 仅 SENDTO/PROPS 用）
    union {
        struct sock_ctx *skctx;// CMD_ADD / CMD_LSN：待加入事件循环的 socket
        struct listener_ctx *lsn;// CMD_ADDACP / CMD_UNLSN / CMD_LSN_UNREF：监听对象
        struct { struct sock_ctx *skctx; netaddr_ctx addr; } conn; // CMD_CONN：连接中 socket + 目标地址(仅 IOCP 用)
        struct { props_cb ppcb; free_cb fcb; void *data; uint64_t number; } props;// CMD_PROPS
        sendto_ctx sendto;// CMD_SENDTO
    } args;
}cmd_ctx;

// 向watcher投递命令。命令必入队,故无返回值：stop 时 watcher 已停止消费,残留由 ev_free 的 drain 兜底释放
void _send_cmd(struct watcher_ctx *watcher, cmd_ctx *cmd);
// 发送CMD_ADDACP命令，通知watcher处理新accept的fd
void _cmd_add_acpfd(struct watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn);
// CMD_ADDACP命令处理：完成 accept fd 的初始化
void _on_cmd_addacp(struct watcher_ctx *watcher, cmd_ctx *cmd);
// 发送CMD_CONN命令，将连接中的sock_ctx交给对应watcher处理
void _cmd_connect(ev_ctx *ctx, struct sock_ctx *skctx, netaddr_ctx *addr);
// CMD_CONN命令处理：在事件循环内注册连接中的socket
void _on_cmd_conn(struct watcher_ctx *watcher, cmd_ctx *cmd);
// 发送CMD_ADD命令
void _cmd_add(struct watcher_ctx *watcher, struct sock_ctx *skctx);
// CMD_ADD命令处理：添加 socket
void _on_cmd_add(struct watcher_ctx *watcher, cmd_ctx *cmd);
#ifndef EV_IOCP
// 发送CMD_LSN命令，通知watcher注册监听socket
void _cmd_listen(struct watcher_ctx *watcher, struct sock_ctx *skctx);
// CMD_LSN命令处理：在事件循环内完成监听注册
void _on_cmd_lsn(struct watcher_ctx *watcher, cmd_ctx *cmd);
// 发送CMD_UNLSN命令，通知watcher取消监听
void _cmd_unlisten(struct watcher_ctx *watcher, SOCKET fd, struct listener_ctx *lsn);
// CMD_UNLSN命令处理：在事件循环内取消监听
void _on_cmd_unlsn(struct watcher_ctx *watcher, cmd_ctx *cmd);
// 发送CMD_LSN_UNREF命令，让 worker 在 _uev_cmd_loop 内减 lsn 占位 ref
// (ev_unlisten 末尾用, 让减占位在 worker 上下文走 qtn 隔离队列)
void _cmd_lsn_unref(struct watcher_ctx *watcher, struct listener_ctx *lsn);
// CMD_LSN_UNREF命令处理：在事件循环内减 lsn 占位 ref (ev_unlisten 末尾发的减占位命令)
void _on_cmd_lsn_unref(struct watcher_ctx *watcher, cmd_ctx *cmd);
#endif //EV_IOCP
// ev_free CMD_STOP命令处理：停止事件循环
void _on_cmd_stop(struct watcher_ctx *watcher, cmd_ctx *cmd);
// ev_sendto CMD_SENDTO命令处理：将UDP数据加入发送队列（校验 fd 类型为 SOCK_DGRAM）
void _on_cmd_sendto(struct watcher_ctx *watcher, cmd_ctx *cmd);
// ev_props CMD_PROPS 自定义
void _on_cmd_props(struct watcher_ctx *watcher, cmd_ctx *cmd);

#endif//CMDS_H_
