#ifndef TASK_KCP_H_
#define TASK_KCP_H_

#include "lib.h"

// KCP 集成测试:conv 握手走 TCP,ikcp_update 由 event 线程 tick 自动驱动,server 一个 task,client 侧六组场景各一个 task。
//   1) server: TCP listen 收 client 上报的 UDP 端口 -> 分配唯一 conv -> server 侧 kcp_start
//      -> TCP 回 conv;UDP 侧 kcp 数据在 on_recvfrom 内按来源端口找到会话 echo 回(noecho 开头的只收不回)。
//   2) client(x3,同一 client task 内 coro_fork): task_udp(0) 由 OS 分配端口 + netaddr_local 取回
//      -> TCP 握手拿 conv -> client 侧 kcp_start -> kcp_synsend 发送并校验 echo(收发由 event tick 驱动)。
//      client 0 额外传非法 mtu=24 覆盖回归:验证 maxpack 不会归零导致该会话永久发送失败。
//      client 1 额外传 mtu=1431655742:超过 KCP_MTU_MAX 被 kcp.c 拒收、保留默认 mtu,会话照常收发。
//      3 个 client 全部 synsend 收到正确 echo -> *ok = 1。
//   3) close: 用 server 从未注册的 conv 指向真实 UDP 端口(永远不会 echo),coro_fork_wait 并发跑
//      synsend 等待者 + 200ms 后 kcp_stop 者;验证 synsend 被 MSG_TYPE_CLOSE 及时唤醒(非等满超时)。
//   4) fifo: 真实握手拿 conv 后,coro_fork_wait 并发多个协程对同一 kcp_ctx synsend;
//      验证 keep=true FIFO 派发下每个协程收到的 echo 精确对应自己发送的内容,不串号。
//   5) synstart: 不依赖真实对端(kcp_start 只在本地登记会话表)。同一 socket 上先后 kcp_synstart
//      两个相同 conv,验证第二次因冲突同步返回失败;以 kcp_start(sess=0) 异步建的会话验证
//      kcp_synsend 立即失败(不进入 _coro_wait)且 copy=0 时不漏释放调用方缓冲区;
//      kcp_stop 后立即 kcp_synstart 重启,验证新 sess 不被上一会话在途的 CLOSE 击穿;
//      再 stop 并让另一句柄占走同 conv,重启必被拒,验证失败路径把 stopped/maxpack 与 sess 一并还原
//      (stopped 未还原则句柄从"已停"变回"在跑",随后的 synsend 会空等满超时而非立即失败);
//      最后另开一个 socket,synsend 挂起期间关掉它,验证 CLOSE 唤醒后 kcp->sess 清零、stopped 置 1。
//      末尾借本组再跑一组对端行为(本 task 内四个 socket):恰好 maxpack(127 片)的消息端到端回显、多 1 字节被拒;
//      mtu 恰为 KCP_MTU_MAX 生效、大 1 保持默认;冒充源地址的段被丢;0 长度消息不堵住其后的消息;
//      kcp_handle 改投后消息归新 task,它退出后到的消息被取出丢掉,改回后不会再冒出来。
//      再借本组跑学习模式(本 task 内三个 socket):port 0 起会话,对端从临时端口先发后双向回显正常,
//      冒充者同 conv 发的被丢;给定 ip 只学端口、通配 ip 连 ip 一起学,两种各跑一遍。
//   6) idle: 两端停表后先发单向消息再 synsend(收包唤醒),再闲 1.5 秒后 synsend(发送唤醒),最后 stop 已停表的会话。
//   7) dead(仅 KCP_TICK_HEAP 为 1 时跑): 本 task 内两个 socket 互为对端,热身 echo 后停掉对端会话,验证在途数据
//      重传到 dead_link 次后会话被拆除,synsend 被 CLOSE(CLOSE_TYPE_ABORT)唤醒而非等满超时,句柄置为 stopped;
//      关闭回调收到的 sess 是该会话的 sess(不是 socket 的 skid);
//      同一 socket 上另有一个对端未建、一直重传的 conv 2,拆掉 conv 1 后再建它的对端,验证 conv 2 仍被 tick 驱动、拿得到回显。
void task_kcp_server_start(loader_ctx *loader, const char *name, uint16_t tcp_port, uint16_t udp_port);
void task_kcp_client_start(loader_ctx *loader, const char *name, uint16_t sv_tcp_port, uint16_t sv_udp_port, int32_t *ok);
void task_kcp_close_start(loader_ctx *loader, const char *name, uint16_t sv_udp_port, int32_t *ok);
void task_kcp_fifo_start(loader_ctx *loader, const char *name, uint16_t sv_tcp_port, uint16_t sv_udp_port, int32_t *ok);
void task_kcp_synstart_start(loader_ctx *loader, const char *name, uint16_t sv_udp_port, int32_t *ok);
void task_kcp_idle_start(loader_ctx *loader, const char *name, uint16_t sv_tcp_port, uint16_t sv_udp_port, int32_t *ok);
void task_kcp_dead_start(loader_ctx *loader, const char *name, int32_t *ok);

#endif//TASK_KCP_H_
