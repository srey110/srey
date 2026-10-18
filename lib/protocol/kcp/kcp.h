#ifndef KCP_H_
#define KCP_H_

#include "event/event.h"
#include "protocol/prots_pub.h"
#include "protocol/kcp/ikcp.h"

// tick 调度方式:1=最小堆早退(会话多且同时到期少时占优;空闲会话摘出堆停表,send / input 时挂回;死链会话拆掉投 CLOSE);
// 0=kcp_map_scan 全量扫描(同时到期会话占多数时占优;不停表、不判死链,仅作对照)
#define KCP_TICK_HEAP 1
#define KCP_MTU_MAX 65507 // mtu 上限:UDP 单报最大载荷(按 IPv4 算,IPv6 也能用),再大满段发不出去
// kcp 会话句柄(值类型,业务持有):仅标识一个会话,不持有会话对象本身。
// 会话对象(含 ikcpcb)在调用 kcp_start 的线程上建好后交给 event 线程入表,此后查/释放都在 event 线程,
// 业务只通过此句柄操作,不会悬空。
typedef struct kcp_ctx {
    uint8_t stopped;    // 1=无存活会话(kcp_init 与 kcp_stop 后置 1);kcp_start 成功发起后置 0
    uint32_t conv;      // 会话号(同一 socket 内唯一,两端约定一致)
    uint64_t sess;      // 会话 sess,每次 kcp_start 传入,kcp_stop 不清;会话级消息都带它(见 kcp_start)
    ev_ctx *netev;      // 事件上下文
    size_t maxpack;     // 单次 kcp_send 消息上限 = 127×(mtu-24),kcp_start 按 config 算,kcp_send 前置校验
    sock_ctx sk;           // 底层 UDP socket 标识(fd + skid)
}kcp_ctx;
// KCP 可调参数,传给 kcp_start(NULL=全用库默认)。
// nodelay/interval/resend/nc 透传 ikcp_nodelay:填 <0 表示该项不改(interval 填 0 会被库钳到 10ms);
// sndwnd/rcvwnd/mtu 填 >0 才生效,否则保持库默认(rcvwnd 库内下限 128,mtu 须 50~KCP_MTU_MAX)
typedef struct kcp_config {
    int32_t nodelay;   // 0 普通(超时重传间隔翻倍) / 1 nodelay(rx_minrto 100→30,间隔每次涨一半)
                       // / 2 同 1 但间隔每次只加 rx_rto/2(线性涨,判死链快);<0 不改
    int32_t interval;  // 内部 flush 间隔 ms,库钳到 [10,5000];<0 不改
    int32_t resend;    // 快速重传阈值(典型 2);<0 不改
    int32_t nc;        // 0 开流控 / 1 关流控(nocwnd);<0 不改
    int32_t sndwnd;    // 发送窗口(默认 32);<=0 不改
    int32_t rcvwnd;    // 接收窗口(默认 128);<=0 不改
    int32_t mtu;       // MTU(默认 1400,须 50~KCP_MTU_MAX,越界告警并保持默认);<=0 不改
}kcp_config;

// 初始化 kcp 模块,注册消息汇(由 prots_init 调用)
void _kcp_init(prot_emit *emit);
// 释放 UDP socket 上的 kcp 上下文及其所有会话(由 prots_udfree 调用)
void _kcp_udfree(ud_cxt *ud);
// UDP 数据解包:ikcp_input 喂入后 ikcp_recv 取完整消息上抛(由 prots_net_recvfrom 调用)。
void _kcp_unpack(ev_ctx *ev, sock_ctx *sk, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud);
// 到期这趟 ikcp_update 的 flush 是否无事可做(成立即停表;放头文件是让单元测试调同一份):已 flush 过
// (cwnd 不再是 0)、无待回 ACK 与探窗应答、发送队列与在途段都空、对端窗口非 0 且探窗计时已清零、
// 没挂拥塞控制;成立时只有 send / input 能打破它。ikcp_flush 新增发包条件(保活等)时须同步这里
static inline int32_t _kcp_idle(const ikcpcb *ikcp) {
    return 0 == ikcp->nsnd_buf
        && 0 == ikcp->nsnd_que
        && 0 == ikcp->ackcount
        && 0 == ikcp->probe
        && 0 != ikcp->rmt_wnd
        && 0 == ikcp->probe_wait
        && 0 != ikcp->updated
        && NULL == ikcp->ccops;
}
// 以 ts_flush 为原点、interval 为步长的格点(可往前推)上第一个不早于 now 的点,结果落在 [now, now+interval)。
// 停表会话挂回时用;放头文件同 _kcp_idle。interval 库内钳到 >=10,不会除零
static inline IUINT32 _kcp_regrid(IUINT32 ts_flush, IUINT32 now, IUINT32 interval) {
    IUINT32 lag = now - ts_flush;
    if ((IINT32)lag >= 0) {
        return now + (interval - lag % interval) % interval;
    }
    return now + (ts_flush - now) % interval;
}
/// <summary>
/// 初始化 kcp 句柄:绑定底层 UDP socket 与会话号,不建立会话(需再调 kcp_start)
/// </summary>
/// <param name="kcp">待初始化的 kcp_ctx(存储由调用方持有)</param>
/// <param name="netev">事件上下文</param>
/// <param name="sk">连接标识</param>
/// <param name="conv">会话号(两端须一致)</param>
void kcp_init(kcp_ctx *kcp, ev_ctx *netev, sock_ctx *sk, uint32_t conv);
/// <summary>
/// 建立 kcp 会话(异步,发起后立即返回):建好后收到的数据以 MSG_TYPE_RECVFROM 推给 handle 所属 task。
/// 建立结果以 MSG_TYPE_HANDSHAKED 推回(erro 为 ERR_OK;conv 冲突或 fd 不是 UDP_KCP 时 ERR_FAILED,
/// 随后再补一条 erro 为 CLOSE_TYPE_NEVERCONN 的 MSG_TYPE_CLOSE),要同步等用 kcp_synstart。
/// 本会话的 RECVFROM / HANDSHAKED / CLOSE 都带本次 sess。会话关闭(kcp_stop、死链、UDP socket 关闭时逐会话补发)
/// 进 task_closed 回调:sess 参数即本会话的 sess,sk 是承载它的 UDP socket;sess 等于 sk->skid 的才是 socket 本身关闭。
/// 对端失联(某段重传到 dead_link 次,库默认 20;仅 KCP_TICK_HEAP 为 1)时会话被拆除并推 MSG_TYPE_CLOSE
/// (erro 为 CLOSE_TYPE_ABORT),重启前须先 kcp_stop;被它唤醒的 kcp_synsend 已把句柄置停。
/// 约束:同一 socket 上 conv 须唯一(由上层协商保证)。
/// </summary>
/// <param name="kcp">已 kcp_init 的句柄</param>
/// <param name="handle">数据到达时推送的目标 task 句柄</param>
/// <param name="sess">本次会话的 sess,用 createid() 生成,每次 kcp_start 传新值:stop 后重启若复用旧 sess,
///   上一会话在途的 CLOSE 会击穿本次等待;kcp_stop / kcp_send / kcp_handle 按 (conv, sess) 认会话,
///   挡住持旧句柄副本的调用落到同 conv 的新会话上。0 仅为兼容:不唤醒等待者,失去上面那层保护
///   (sess 都为 0 的会话互相匹配),会话关闭时也认不出是哪个会话</param>
/// <param name="ip">对端 IP;port 为 0 且 ip 为通配地址(0.0.0.0 / ::)时 ip 也从首包学</param>
/// <param name="port">对端端口;0 为学习模式:首个 conv 匹配(ip 非通配时还须 ip 相符)的数据报来源定为对端,
///   之后严格校验。学到之前本端发出的段被丢掉(学到后靠重传补上,故应让对端先发),这期间猜中 conv 的任何来源
///   都能抢先占住;只在拿不到对端实际地址(如对端在 NAT 后)时用</param>
/// <param name="cfg">KCP 可调参数;NULL 用库默认(见 kcp_config)</param>
/// <returns>ERR_OK 请求已发起,不代表会话已建好;ERR_FAILED 本地校验失败或未能发起,sess / maxpack 不变。
///   句柄 stopped 为 0 时先隐式 kcp_stop 旧会话再起新的,失败时 stopped 保持被它置的 1</returns>
int32_t kcp_start(kcp_ctx *kcp, name_t handle, uint64_t sess,
                  const char *ip, uint16_t port, const kcp_config *cfg);
/// <summary>
/// 停止并释放 kcp 会话:出表后向推送目标投 MSG_TYPE_CLOSE(erro 为 CLOSE_TYPE_LOCAL,会进 task_closed 回调,
/// sess 同 kcp_start),再释放会话对象。句柄上的 sess 不清。句柄已停时什么也不做;会话已被拆掉(如死链)时 event 线程上静默忽略
/// </summary>
/// <param name="kcp">kcp_ctx</param>
void kcp_stop(kcp_ctx *kcp);
/// <summary>
/// 变更会话的数据推送目标 task 句柄(之后的 RECVFROM 与 CLOSE 都投给它)。
/// 目标在此之后退出时,该会话收到的消息被丢弃并限频告警
/// </summary>
/// <param name="kcp">kcp_ctx</param>
/// <param name="handle">新的目标 task 句柄</param>
/// <returns>ERR_OK 命令已投递,不代表会话已找到(会话已拆或 sess 不符时 event 线程上只打告警);
///   句柄已停或 sk 无效 ERR_FAILED</returns>
int32_t kcp_handle(kcp_ctx *kcp, name_t handle);
/// <summary>
/// 发送数据:交 KCP 可靠传输,实际发包由 event 线程 tick 周期驱动。
/// 对端响应到达后以 kcp_start 时传入的 sess 唤醒等待协程(供 kcp_synsend 用)。
/// </summary>
/// <param name="kcp">kcp_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">数据长度</param>
/// <param name="copy">1 拷贝数据;0 转移所有权(框架内部释放)</param>
/// <returns>ERR_OK 已交给 event 线程,不代表送达;句柄已停、lens 为 0 或超过 kcp->maxpack(见 kcp_start)、
///   sk 无效返回 ERR_FAILED,copy 为 0 时 data 已释放</returns>
int32_t kcp_send(kcp_ctx *kcp, void *data, size_t lens, int32_t copy);

#endif// KCP_H_
