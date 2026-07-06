#ifndef TASK_KCP_H_
#define TASK_KCP_H_

#include "lib.h"

// KCP 集成测试:conv 握手走 TCP,ikcp_update 由 event 线程 tick 自动驱动,server 一个 task,client 侧三组场景各一个 task。
//   1) server: TCP listen 收 client 上报的 UDP 端口 -> 分配唯一 conv -> server 侧 kcp_start
//      -> TCP 回 conv;UDP 侧 kcp 数据在 on_recvfrom 内按来源端口找到会话 echo 回。
//   2) client(x3,同一 client task 内 coro_fork): task_udp(0) 由 OS 分配端口 + netaddr_local 取回
//      -> TCP 握手拿 conv -> client 侧 kcp_start -> kcp_synsend 发送并校验 echo(收发由 event tick 驱动)。
//      client 0 额外传非法 mtu=24 覆盖回归:验证 maxpack 不会归零导致该会话永久发送失败。
//      3 个 client 全部 synsend 收到正确 echo -> *ok = 1。
//   3) close: 用 server 从未注册的 conv 指向真实 UDP 端口(永远不会 echo),coro_fork_wait 并发跑
//      synsend 等待者 + 200ms 后 kcp_stop 者;验证 synsend 被 MSG_TYPE_CLOSE 及时唤醒(非等满超时)。
//   4) fifo: 真实握手拿 conv 后,coro_fork_wait 并发多个协程对同一 kcp_ctx synsend;
//      验证 keep=true FIFO 派发下每个协程收到的 echo 精确对应自己发送的内容,不串号。
//   5) synstart: 不依赖真实对端(kcp_start 只在本地登记会话表)。同一 socket 上先后 kcp_synstart
//      两个相同 conv,验证第二次因冲突同步返回失败;再用 sess=0 的 kcp_ctx 验证 kcp_synstart/kcp_synsend
//      均立即失败(不进入 _coro_wait),且 copy=0 时不漏释放调用方缓冲区。
void task_kcp_server_start(loader_ctx *loader, const char *name, uint16_t tcp_port, uint16_t udp_port);
void task_kcp_client_start(loader_ctx *loader, const char *name, uint16_t sv_tcp_port, uint16_t sv_udp_port, int32_t *ok);
void task_kcp_close_start(loader_ctx *loader, const char *name, uint16_t sv_udp_port, int32_t *ok);
void task_kcp_fifo_start(loader_ctx *loader, const char *name, uint16_t sv_tcp_port, uint16_t sv_udp_port, int32_t *ok);
void task_kcp_synstart_start(loader_ctx *loader, const char *name, uint16_t sv_udp_port, int32_t *ok);

#endif//TASK_KCP_H_
