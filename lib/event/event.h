#ifndef EVENT_H_ 
#define EVENT_H_

#include "event/evpub.h"
#include "event/evssl.h"
#include "thread/thread.h"

/// <summary>
/// 网络初始化
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="nthreads">线程数</param>
/// <param name="hooks">网络线程的 init/exit 钩子,NULL 表示不挂钩子</param>
void ev_init(ev_ctx *ctx, uint32_t nthreads, const thread_hooks *hooks);
/// <summary>
/// 网络释放
/// </summary>
/// <param name="ctx">ev_ctx</param>
void ev_free(ev_ctx *ctx);
/// <summary>
/// 监听。启用 SSL 时（evssl != NULL）业务须等 ssl握手完成回调后才能 ev_send
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="evssl">evssl_ctx, NULL 不使用,不为NULL默认启用ssl</param>
/// <param name="ip">监听IP(0.0.0.0 - ::  127.0.0.1 - ::1)</param>
/// <param name="port">监听端口</param>
/// <param name="cbs">回调函数</param>
/// <param name="ud">用户数据</param>
/// <param name="id">监听ID</param>
/// <returns>ERR_OK 成功</returns>
int32_t ev_listen(ev_ctx *ctx, struct evssl_ctx *evssl, const char *ip, const uint16_t port,
    cbs_ctx *cbs, ud_cxt *ud, uint64_t *id);
/// <summary>
/// 链接。业务须等连接回调(conn_cb)后才能 ev_send;启用 SSL 时（evssl != NULL）还须等 ssl握手完成回调后才能 ev_send
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="evssl">evssl_ctx, NULL 不使用,不为NULL默认启用ssl</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="cbs">回调函数</param>
/// <param name="ud">用户数据</param>
/// <param name="fd">SOCKET</param>
/// <param name="skid">链接ID</param>
/// <returns>ERR_OK 成功</returns>
int32_t ev_connect(ev_ctx *ctx, struct evssl_ctx *evssl, const char *ip, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud,
    SOCKET *fd, uint64_t *skid);
/// <summary>
/// 切换为SSL链接。启用 SSL 时 业务须等 ssl握手完成回调后才能 ev_send
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="client">1 作为客户端, 0 作为服务端</param>
/// <param name="evssl">evssl_ctx, 必须非 NULL (NULL 时返 ERR_FAILED)</param>
/// <returns>ERR_OK 成功</returns>
int32_t ev_ssl(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t client, struct evssl_ctx *evssl);
/// <summary>
/// UDP
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="cbs">回调函数</param>
/// <param name="ud">用户数据</param>
/// <param name="fd">SOCKET</param>
/// <param name="skid">链接ID</param>
/// <returns>ERR_OK 成功</returns>
int32_t ev_udp(ev_ctx *ctx, const char *ip, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud, SOCKET *fd, uint64_t *skid);
/// <summary>
/// TCP发送数据
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="data">要发送的数据</param>
/// <param name="len">数据长度,须 > 0(为 0 时返 ERR_FAILED)</param>
/// <param name="copy">1 拷贝数据 不自动释放, 0不拷贝数据 自动释放</param>
/// <returns>ERR_OK 成功</returns>
int32_t ev_send(ev_ctx *ctx, SOCKET fd, uint64_t skid, void *data, size_t len, int32_t copy);
/// <summary>
/// 多播发送：将同一份 data 零拷贝广播给 N 个 TCP fd。内部分配 shared_data(引用计数=有效 fd 数),
/// 给每个 fd 投一条 CMD_SEND_MULTI 命令；各 fd 发送完成或失败时 ref--, 归 0 才真正释放 data。
/// 支持 fds[i]=INVALID_SOCK 占位（跳过不投递）；有效 fd 全为 INVALID_SOCK 时直接按 copy 语义清理 data 返回 ERR_FAILED。
/// copy=1 内部 MALLOC+memcpy 一份给 pack 持有；copy=0 直接持有 data 所有权(业务不可再用)。
/// 仅适用 TCP；UDP 仍走 ev_sendto。
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fds">SOCKET 数组，长度 n</param>
/// <param name="skids">连接 ID 数组，长度 n，与 fds 一一配对</param>
/// <param name="n">数组长度</param>
/// <param name="data">要发送的数据</param>
/// <param name="len">数据长度,须 > 0(为 0 时返 ERR_FAILED)</param>
/// <param name="copy">1 拷贝数据 不自动释放, 0 不拷贝 自动释放</param>
/// <returns>ERR_OK 成功(至少 1 个有效 fd 投递成功);ERR_FAILED 全部无效 fd</returns>
int32_t ev_send_multi(ev_ctx *ctx, SOCKET fds[], uint64_t skids[], int32_t n,
                      void *data, size_t len, int32_t copy);
/// <summary>
/// UDP发送数据
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="data">要发送的数据</param>
/// <param name="len">数据长度</param>
/// <param name="copy">1 不自动释放, 0 自动释放</param>
/// <returns>ERR_OK 请求成功</returns>
int32_t ev_sendto(ev_ctx *ctx, SOCKET fd, uint64_t skid, const char *ip, const uint16_t port,
    void *data, size_t len, int32_t copy);
/// <summary>
/// UDP socket 加入多播组(IPv4/IPv6 自动按 socket family 分支)。
/// 加入后该 socket 会收到发往 group_ip:port 的多播包,通过 cbs.rf_cb 上报。
/// 接收端先 ev_udp("0.0.0.0", PORT)/ev_udp("::", PORT) 绑定端口,再 ev_udp_join 加组;
/// 同一 socket 可加入多个组(各调一次);离开用 ev_udp_leave。
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">已通过 ev_udp 创建的 UDP socket(必须 SOCK_DGRAM)</param>
/// <param name="skid">连接 skid</param>
/// <param name="group_ip">多播组地址。IPv4 须在 224.0.0.0/4 段(例 "239.0.0.1"); IPv6 须 ff00::/8 段(例 "ff02::1")</param>
/// <param name="iface_str">接收网卡。IPv4 走网卡 IP 字符串(例 "192.168.1.100"); IPv6 走接口名(例 "en0"); NULL 走系统默认</param>
/// <returns>ERR_OK 成功</returns>
int32_t ev_udp_join(ev_ctx *ctx, SOCKET fd, uint64_t skid,
                    const char *group_ip, const char *iface_str);
/// <summary>
/// UDP socket 离开多播组。配对 ev_udp_join,参数语义相同。
/// </summary>
int32_t ev_udp_leave(ev_ctx *ctx, SOCKET fd, uint64_t skid,
                     const char *group_ip, const char *iface_str);
/// <summary>
/// 设置 UDP 多播 TTL(IPv4 IP_MULTICAST_TTL) / Hop Limit(IPv6 IPV6_MULTICAST_HOPS)。
/// 默认 1 仅本网段;设 32 跨多网段;255 跨广域。仅影响该 socket 后续 ev_sendto 到多播地址的包,不影响单播。
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">UDP socket</param>
/// <param name="skid">连接 skid</param>
/// <param name="ttl">1-255</param>
/// <returns>ERR_OK 成功</returns>
int32_t ev_udp_ttl(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint8_t ttl);
/// <summary>
/// 设置 UDP 多播本机回环(IP_MULTICAST_LOOP / IPV6_MULTICAST_LOOP)。
/// 默认 1(发出去自己也能收到);0 时本机不收自己发的多播包。同 task 既发又收时设 0 避免回环。
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">UDP socket</param>
/// <param name="skid">连接 skid</param>
/// <param name="enable">1=收回环 0=不收</param>
/// <returns>ERR_OK 成功</returns>
int32_t ev_udp_loop(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t enable);
/// <summary>
/// 关闭链接
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="immed">
///    0=优雅关闭(等 send queue 发完, TCP only; UDP 退化为立即关);
///    1=立即关闭(丢弃未发数据)
/// </param>
void ev_close(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t immed);
/// <summary>
/// 取消监听
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="id">监听ID</param>
void ev_unlisten(ev_ctx *ctx, uint64_t id);
/// <summary>
/// 设置ud_cxt的数据包类型
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="pktype">数据包类型 pack_type</param>
/// <returns>ERR_OK 成功，stop 非0失败</returns>
int32_t ev_ud_pktype(ev_ctx *ctx, SOCKET fd, uint64_t skid, subtype_t pktype);
/// <summary>
/// 设置ud_cxt的状态
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="status">状态</param>
/// <returns>ERR_OK 成功，stop 非0失败</returns>
int32_t ev_ud_status(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint8_t status);
/// <summary>
/// 设置ud_cxt的session
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="sess">session</param>
/// <returns>ERR_OK 成功，stop 非0失败</returns>
int32_t ev_ud_sess(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint64_t sess);
/// <summary>
/// 设置ud_cxt的任务句柄
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="handle">任务句柄</param>
/// <returns>ERR_OK 成功，stop 非0失败</returns>
int32_t ev_ud_handle(ev_ctx *ctx, SOCKET fd, uint64_t skid, name_t handle);
/// <summary>
/// 设置ud_cxt的extra
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="extra">extra</param>
/// <returns>ERR_OK 成功，stop 非0失败</returns>
int32_t ev_ud_context(ev_ctx *ctx, SOCKET fd, uint64_t skid, void *extra);
/// <summary>
/// 设置子协议的ud_cxt的extra,websocket
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="extra">extra</param>
/// <returns>ERR_OK 成功，stop 非0失败</returns>
int32_t ev_ud_seccontext(ev_ctx *ctx, SOCKET fd, uint64_t skid, void *extra);

#endif//EVENT_H_
