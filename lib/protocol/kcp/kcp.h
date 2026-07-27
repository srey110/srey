#ifndef KCP_H_
#define KCP_H_

#include "event/event.h"
#include "protocol/prots_pub.h"
#include "protocol/kcp/ikcp.h"

// tick 调度方式:1=最小堆早退(会话多且同时到期少时占优) 0=hashmap_scan 全量扫描(同时到期会话占多数时占优)
#define KCP_TICK_HEAP 1
// kcp 会话句柄(值类型,业务持有):仅标识一个会话,不持有会话对象本身。
// 会话对象(含 ikcpcb)由框架在 event 线程内建/查/释放,业务只通过此句柄操作,不会悬空。
typedef struct kcp_ctx {
    uint8_t stopped;    // 1=无存活会话(kcp_init 与 kcp_stop 后置 1);kcp_start 成功发起后置 0
    uint32_t conv;      // 会话号(同一 socket 内唯一,两端约定一致)
    uint64_t sess;      // 唤醒 sess,每次 kcp_start 传入,会话内不变;0 表示不唤醒(纯异步)
    ev_ctx *netev;      // 事件上下文
    size_t maxpack;     // 单次 kcp_send 消息上限 = 127×(mtu-24),kcp_start 按 config 算,kcp_send 前置校验
    sk_id sk;           // 底层 UDP socket 标识(fd + skid)
}kcp_ctx;
// KCP 可调参数,传给 kcp_start(NULL=全用库默认)。
// nodelay/interval/resend/nc 透传 ikcp_nodelay:填 <0 表示该项不改(interval 填 0 会被库钳到 10ms);
// sndwnd/rcvwnd/mtu 填 >0 才生效,否则保持库默认(rcvwnd 库内下限 128,mtu 须 >=50)
typedef struct kcp_config {
    int32_t nodelay;   // 0 普通 / 1 nodelay(降 rx_minrto 100→30);<0 不改
    int32_t interval;  // 内部 flush 间隔 ms,库钳到 [10,5000];<0 不改
    int32_t resend;    // 快速重传阈值(典型 2);<0 不改
    int32_t nc;        // 0 开流控 / 1 关流控(nocwnd);<0 不改
    int32_t sndwnd;    // 发送窗口(默认 32);<=0 不改
    int32_t rcvwnd;    // 接收窗口(默认 128);<=0 不改
    int32_t mtu;       // MTU(默认 1400,须 >=50);<=0 不改
}kcp_config;

// 初始化 kcp 模块,注册消息汇(由 prots_init 调用)
void _kcp_init(prot_emit *emit);
// 释放 UDP socket 上的 kcp 上下文及其所有会话(由 prots_udfree 调用)
void _kcp_udfree(ud_cxt *ud);
// UDP socket 关闭时的清理回调，等同于 _kcp_udfree(由 prots_closed 调用)
void _kcp_fd_closed(ud_cxt *ud);
// UDP 数据解包:ikcp_input 喂入后 ikcp_recv 取完整消息上抛(由 prots_net_recvfrom 调用)
void _kcp_unpack(SOCKET fd, uint64_t skid,
                 char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud);
/// <summary>
/// 初始化 kcp 句柄:绑定底层 UDP socket 与会话号,不建立会话(需再调 kcp_start)
/// </summary>
/// <param name="kcp">待初始化的 kcp_ctx(存储由调用方持有)</param>
/// <param name="netev">事件上下文</param>
/// <param name="fd">底层 UDP socket 句柄</param>
/// <param name="skid">连接 ID</param>
/// <param name="conv">会话号(两端须一致)</param>
void kcp_init(kcp_ctx *kcp, ev_ctx *netev, SOCKET fd, uint64_t skid, uint32_t conv);
/// <summary>
/// 建立 kcp 会话(异步,发起后立即返回):在 event 线程创建会话对象并加入该 socket 的会话表;
/// 此后该会话收到的数据以 MSG_TYPE_RECVFROM 推送给 handle 所属 task。
/// event 线程上的实际结果(成功,或因 conv 冲突失败)以 MSG_TYPE_HANDSHAKED 异步推给 handle 所属 task
/// (msg.subtype 为 PACK_UDP_KCP,msg.sess 为本次传入的 sess,msg.erro 为 ERR_OK/ERR_FAILED);
/// 需要同步拿到该结果请用 kcp_synstart。
/// 约束:同一 socket 上 conv 须唯一(由上层协商保证)。
/// </summary>
/// <param name="kcp">已 kcp_init 的句柄</param>
/// <param name="handle">数据到达时推送的目标 task 句柄</param>
/// <param name="sess">本次会话的唤醒 sess(调用方生成,如 createid());0 表示不唤醒。
///   每次 kcp_start 须传新值:stop 后重启若复用旧 sess,上一会话在途的 CLOSE 会击穿本次等待</param>
/// <param name="ip">对端 IP</param>
/// <param name="port">对端端口</param>
/// <param name="cfg">KCP 可调参数;NULL 用库默认(见 kcp_config)</param>
/// <returns>ERR_OK 请求已发起;ERR_FAILED 本地校验失败或请求未能发起(不代表 event 线程上的会话建立结果)。
///   同一句柄 stopped 为 0 时先隐式 kcp_stop 再起新会话:旧会话按正规流程拆除并投 CLOSE,
///   故 sess 键不会指向未入表的新会话而把上一个会话变成无法 stop 的孤儿。
///   之所以不改为拒绝:event 线程拒建会话(conv 冲突 / fd 类型错)时 stopped 已停在 0 而会话从未存在,
///   拒绝会让这种句柄永久起不来——async 调用方没有协程去消费那条合成 CLOSE 来复位 stopped。
///   会话已不存在时该隐式 stop 是空操作(仅一条 can't find conv 的 WARN)。
///   返 ERR_FAILED 时不改动 sess / maxpack;stopped 若已被上述隐式 stop 置 1 则保持 1
///   (此时确实无存活会话,不会把句柄卡在"在跑"上)</returns>
int32_t kcp_start(kcp_ctx *kcp, name_t handle, uint64_t sess,
                  const char *ip, uint16_t port, const kcp_config *cfg);
/// <summary>
/// 停止并释放 kcp 会话(从会话表移除并释放会话对象)
/// </summary>
/// <param name="kcp">kcp_ctx</param>
void kcp_stop(kcp_ctx *kcp);
/// <summary>
/// 变更会话的数据推送目标 task 句柄
/// </summary>
/// <param name="kcp">kcp_ctx</param>
/// <param name="handle">新的目标 task 句柄</param>
/// <returns>ERR_OK 成功</returns>
int32_t kcp_handle(kcp_ctx *kcp, name_t handle);
/// <summary>
/// 发送数据:交 KCP 可靠传输,实际发包由 event 线程 tick 周期驱动。
/// 对端响应到达后以 kcp_start 时传入的 sess 唤醒等待协程(供 kcp_synsend 用)。
/// </summary>
/// <param name="kcp">kcp_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">数据长度</param>
/// <param name="copy">1 拷贝数据;0 转移所有权(框架内部释放)</param>
/// <returns>ERR_OK 成功;lens 为 0 或超过 kcp->maxpack(见 kcp_start)返回 ERR_FAILED</returns>
int32_t kcp_send(kcp_ctx *kcp, void *data, size_t lens, int32_t copy);

#endif// KCP_H_
