#ifndef EVENT_H_ 
#define EVENT_H_

#include "event/evpub.h"
#include "event/evssl.h"
#include "thread/thread.h"

/// <summary>
/// 网络初始化
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="nthreads">网络线程数；0 表示按 CPU 核心数.KQUEUE 多线程会恶化</param>
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
/// <param name="ip">监听IP。"::" 只收 IPv6(强制 IPV6_V6ONLY,不随平台默认变),要同时收两种就
///   "0.0.0.0" 与 "::" 各监听一次;本机对应 "127.0.0.1" / "::1"</param>
/// <param name="port">监听端口</param>
/// <param name="cbs">回调函数; 必须非 NULL 且 r_cb 非 NULL(UDP 那面是 rf_cb)</param>
/// <param name="ud">用户数据; 失败时由本函数经 cbs->ud_free 释放, 调用方不必再管。
///   cbs 为 NULL 时取不到 ud_free, 这一档 ud 仍归调用方</param>
/// <param name="id">监听ID</param>
/// <returns>ERR_OK 成功; cbs / r_cb 为空、ev_free 已开始、地址解析失败都返 ERR_FAILED,
///   其中只有地址解析那档会落日志</returns>
int32_t ev_listen(ev_ctx *ctx, struct evssl_ctx *evssl, const char *ip, const uint16_t port,
    cbs_ctx *cbs, ud_cxt *ud, uint64_t *id);
/// <summary>
/// 链接。业务须等连接回调(conn_cb)后才能 ev_send;启用 SSL 时（evssl != NULL）还须等 ssl握手完成回调后才能 ev_send
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="evssl">evssl_ctx, NULL 不使用,不为NULL默认启用ssl</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="cbs">回调函数; 前置条件同 ev_listen</param>
/// <param name="ud">用户数据; 失败时的释放同 ev_listen</param>
/// <param name="setsess">是否设置sess</param>
/// <param name="index">连接落在哪个 event 线程：INVALID_INDEX 按 fd 分配，否则 [0, nthreads)，越界断言</param>
/// <param name="sk">连接标识</param>
/// <returns>ERR_OK 成功; 失败情形同 ev_listen</returns>
int32_t ev_connect(ev_ctx *ctx, struct evssl_ctx *evssl, const char *ip, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud,
    int32_t setsess, int32_t index, sock_ctx *sk);
/// <summary>
/// 切换为SSL链接。启用 SSL 时 业务须等 ssl握手完成回调后才能 ev_send
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="client">1 作为客户端, 0 作为服务端</param>
/// <param name="evssl">evssl_ctx, 必须非 NULL (NULL 时返 ERR_FAILED)</param>
/// <returns>ERR_OK 成功</returns>
int32_t ev_ssl(ev_ctx *ctx, sock_ctx *sk, int32_t client, struct evssl_ctx *evssl);
/// <summary>
/// 发送升级前的最后一条明文，并在它写出后把本端切为 SSL。两步在事件线程内一次做完。
/// 带内升级（回一条明文告知对端可以握手，再切 SSL）必须用它：拆成 ev_send + ev_ssl 两条命令时，
/// 发起方可能在两者之间被抢占，对端的握手报文会被当明文协议解析并按超长包断连。
/// 升级的准入条件同 ev_ssl；被拒时尾包已经发出，不回滚。
/// 本包之后到 SSL 握手完成回调之前不得再 ev_send（同 ev_ssl）。
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="data">明文尾包</param>
/// <param name="len">数据长度, 不可为 0</param>
/// <param name="copy">1 拷贝数据 不自动释放, 0 不拷贝数据 自动释放</param>
/// <param name="client">1 作为客户端, 0 作为服务端</param>
/// <param name="evssl">evssl_ctx, 必须非 NULL (NULL 时返 ERR_FAILED)</param>
/// <returns>ERR_OK 命令已投递; 参数非法或未启用 SSL 时返 ERR_FAILED, 此时 copy 为 0 的 data 已被释放。
///   命令在事件线程内的执行结果不回传</returns>
int32_t ev_send_ssl(ev_ctx *ctx, sock_ctx *sk, void *data, size_t len,
    int32_t copy, int32_t client, struct evssl_ctx *evssl);
/// <summary>
/// 对已完成握手的 TLS1.3 连接排程一次 KeyUpdate。非 TLS1.3、握手未完成或非 TCP 时静默忽略。
/// 排程不等于发出：报文要等该连接下一次 SSL_write 才上线路（口径见 evssl_keyupdate），
/// 故空闲连接上调它不会有任何报文，调用方须自己再发点数据把它带出去。
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="updatetype">SSL_KEY_UPDATE_NOT_REQUESTED 单向 / SSL_KEY_UPDATE_REQUESTED 双向</param>
/// <returns>ERR_OK 命令已投递；WITH_SSL 关闭时返 ERR_FAILED。命令在事件线程内的执行结果不回传</returns>
int32_t ev_keyupdate(ev_ctx *ctx, sock_ctx *sk, int32_t updatetype);
/// <summary>
/// UDP
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="ip">绑定IP。"::" 只收 IPv6(强制 IPV6_V6ONLY),要同时收两种就绑两个 socket。
///   多播时组地址须与此同族,详见 ev_udp_join</param>
/// <param name="port">端口</param>
/// <param name="cbs">回调函数; 前置条件同 ev_listen, 但 UDP 认的是 rf_cb</param>
/// <param name="ud">用户数据; 失败时的释放同 ev_listen</param>
/// <param name="index">socket 落在哪个 event 线程，口径同 ev_connect</param>
/// <param name="sk">连接标识</param>
/// <returns>ERR_OK 成功; 失败情形同 ev_listen</returns>
int32_t ev_udp(ev_ctx *ctx, const char *ip, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud,
    int32_t index, sock_ctx *sk);
/// <summary>
/// TCP发送数据
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="data">要发送的数据</param>
/// <param name="len">数据长度,须 > 0(为 0 时返 ERR_FAILED)</param>
/// <param name="copy">1 拷贝数据 不自动释放, 0不拷贝数据 自动释放</param>
/// <returns>ERR_OK 成功</returns>
int32_t ev_send(ev_ctx *ctx, sock_ctx *sk, void *data, size_t len, int32_t copy);
/// <summary>
/// 多播发送：将同一份 data 零拷贝广播给 N 个 TCP fd
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fds">SOCKET 数组，长度 n</param>
/// <param name="skids">连接 ID 数组，长度 n，与 fds 一一配对</param>
/// <param name="n">数组长度</param>
/// <param name="data">要发送的数据</param>
/// <param name="len">数据长度,须 > 0(为 0 时返 ERR_FAILED)</param>
/// <param name="copy">1 拷贝数据 不自动释放, 0 不拷贝 自动释放</param>
/// <returns>ERR_OK 成功(至少 1 个有效 fd 投递成功);ERR_FAILED 全部无效 fd</returns>
int32_t ev_send_multi(ev_ctx *ctx, sock_ctx sks[], int32_t n, void *data, size_t len, int32_t copy);
/// <summary>
/// UDP发送数据
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="data">要发送的数据</param>
/// <param name="len">数据长度</param>
/// <param name="copy">1 不自动释放, 0 自动释放</param>
/// <returns>ERR_OK 请求成功</returns>
int32_t ev_sendto(ev_ctx *ctx, sock_ctx *sk, const char *ip, const uint16_t port,
    void *data, size_t len, int32_t copy);
/// <summary>
/// UDP发送数据,直接指定目标地址
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="addr">目标地址</param>
/// <param name="data">要发送的数据</param>
/// <param name="len">数据长度</param>
/// <param name="copy">1 不自动释放, 0 自动释放</param>
/// <returns>ERR_OK 请求成功</returns>
int32_t ev_sendto_addr(ev_ctx *ctx, sock_ctx *sk, netaddr_ctx *addr,
    void *data, size_t len, int32_t copy);
/// <summary>
/// UDP socket 加入多播组(按 group_ip 的 family 选 IPv4 / IPv6 选项)。
/// 加入后该 socket 会收到发往 group_ip:port 的多播包,通过 cbs.rf_cb 上报。
/// 接收端先 ev_udp 绑定端口(IPv4 组绑 "0.0.0.0"、IPv6 组绑 "::"),再 ev_udp_join 加组;
/// 同一 socket 可加入多个组(各调一次);离开用 ev_udp_leave。
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="group_ip">多播组地址,**必须与 socket 绑定地址同族**,不同族或不是合法 IP 直接返 ERR_FAILED
///   (IPv6 socket 强制 v6only 后收不到 IPv4 流量)。IPv4 须在 224.0.0.0/4 段(例 "239.0.0.1"); IPv6 须 ff00::/8 段(例 "ff02::1")</param>
/// <param name="iface_str">接收网卡。IPv4 走网卡 IP 字符串(例 "192.168.1.100"); IPv6 走接口名(例 "en0"); NULL 走系统默认</param>
/// <returns>ERR_OK 只表示参数合法且命令已入队。调用方契约违反同步返 ERR_FAILED:fd 无效、group_ip 为
///   NULL / 过长 / 不是合法 IP / 与 socket 不同族 / 不是多播地址。setsockopt 本身在事件线程执行,成败不回传——
///   失败只有一条 LOG_ERROR。下面 leave / ttl / loop 同此契约</returns>
int32_t ev_udp_join(ev_ctx *ctx, sock_ctx *sk, const char *group_ip, const char *iface_str);
/// <summary>
/// UDP socket 离开多播组。配对 ev_udp_join,未加入过的组也允许调用(setsockopt 报错落日志)。
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="group_ip">多播组地址,语义同 ev_udp_join 的同名参数</param>
/// <param name="iface_str">接收网卡,语义同 ev_udp_join 的同名参数</param>
/// <returns>ERR_OK 只表示参数合法且命令已入队,契约同 ev_udp_join</returns>
int32_t ev_udp_leave(ev_ctx *ctx, sock_ctx *sk, const char *group_ip, const char *iface_str);
/// <summary>
/// 设置 UDP 多播 TTL(IPv4 IP_MULTICAST_TTL) / Hop Limit(IPv6 IPV6_MULTICAST_HOPS)。
/// 默认 1 仅本网段;设 32 跨多网段;255 跨广域。仅影响该 socket 后续 ev_sendto 到多播地址的包,不影响单播。
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="ttl">0-255;0 只到本机</param>
/// <returns>ERR_OK 只表示参数合法且命令已入队,契约同 ev_udp_join</returns>
int32_t ev_udp_ttl(ev_ctx *ctx, sock_ctx *sk, uint8_t ttl);
/// <summary>
/// 设置 UDP 多播本机回环(IP_MULTICAST_LOOP / IPV6_MULTICAST_LOOP)。
/// 默认 1(发出去自己也能收到);0 时本机不收自己发的多播包。同 task 既发又收时设 0 避免回环。
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="enable">1=收回环 0=不收</param>
/// <returns>ERR_OK 只表示参数合法且命令已入队,契约同 ev_udp_join</returns>
int32_t ev_udp_loop(ev_ctx *ctx, sock_ctx *sk, int32_t enable);
/// <summary>
/// 关闭链接。关闭前对 send queue 冲一次：能写进内核的送达，写不进去的未发数据连同连接一起丢弃
/// 并落 WARN。没有"等发完再关"的模式——要保证大块数据送达，须自行确认对端已收齐再调用。
/// 已连通的 TCP 连接此后只关写方向，对端再发来的数据由事件层读掉丢弃，等对端关闭、CLOSE_LINGER_MS
/// 到期或丢满 CLOSE_LINGER_BYTES 才真正关 fd，免得对端收到 RST 丢掉最后的响应；关闭回调不等这一段
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="sk">连接标识</param>
/// <returns>ERR_OK 成功</returns>
int32_t ev_close(ev_ctx *ctx, sock_ctx *sk);
/// <summary>
/// 取消监听
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="id">监听ID</param>
void ev_unlisten(ev_ctx *ctx, uint64_t id);
/// <summary>
/// 在事件循环线程内对连接的 ud_cxt 执行自定义操作
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="ppcb">操作回调,仅当 fd/skid 有效时在事件线程内被调用,签名 (watcher, evsk, data, number)</param>
/// <param name="fcb">data 释放回调,可为 NULL;仅在 data 非 NULL **且 ppcb 返回非 0** 时被调用。
///   ppcb 返回 0 表示它已接管 data 所有权(如 _ev_send 把 data 转交发送队列),此时不会调 fcb;
///   fd/skid 失配等未执行 ppcb 的失败路径一律调 fcb</param>
/// <param name="data">传给 ppcb 的指针参数(其生命周期见 fcb 说明),无指针载荷时传 NULL</param>
/// <param name="number">传给 ppcb 的整数参数,无整数载荷时传 0</param>
/// <returns>ERR_OK 请求成功;仅 fd 为 INVALID_SOCK 或 ppcb 为 NULL 时返回 ERR_FAILED。
///   命令入队恒成功,ev_free 已启动时同样返 ERR_OK(命令留在队里由 drain 清理),调用方无法据返回值判断 ev 是否在停</returns>
int32_t ev_props(ev_ctx *ctx, sock_ctx *sk,
                 props_cb ppcb, free_cb fcb, void *data, uint64_t number);
/// <summary>
/// 设置ud_cxt的数据包类型
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="pktype">数据包类型 pack_type</param>
/// <returns>ERR_OK 成功;仅 fd 为 INVALID_SOCK 时返回 ERR_FAILED。命令入队恒成功,ev_free 已启动时同样返 ERR_OK</returns>
int32_t ev_ud_pktype(ev_ctx *ctx, sock_ctx *sk, subtype_t pktype);
/// <summary>
/// 设置ud_cxt的状态
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="status">状态</param>
/// <returns>ERR_OK 成功;仅 fd 为 INVALID_SOCK 时返回 ERR_FAILED。命令入队恒成功,ev_free 已启动时同样返 ERR_OK</returns>
int32_t ev_ud_status(ev_ctx *ctx, sock_ctx *sk, uint8_t status);
/// <summary>
/// 设置ud_cxt的session
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="sess">session</param>
/// <returns>ERR_OK 成功;仅 fd 为 INVALID_SOCK 时返回 ERR_FAILED。命令入队恒成功,ev_free 已启动时同样返 ERR_OK</returns>
int32_t ev_ud_sess(ev_ctx *ctx, sock_ctx *sk, uint64_t sess);
/// <summary>
/// 设置ud_cxt的任务句柄
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="handle">任务句柄</param>
/// <returns>ERR_OK 成功;仅 fd 为 INVALID_SOCK 时返回 ERR_FAILED。命令入队恒成功,ev_free 已启动时同样返 ERR_OK</returns>
int32_t ev_ud_handle(ev_ctx *ctx, sock_ctx *sk, name_t handle);
/// <summary>
/// 设置ud_cxt的extra
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="extra">extra，设置成功后所有权转移给 ud_cxt->context。不释放原值——只可用于
/// context 为空的连接，已挂上下文的连接上调用会让原值失去最后一个持有者</param>
/// <param name="fcb">extra 的释放回调，可为 NULL(不释放)。凡没能真的设上一律用它回收 extra：
/// fd 非法、命令到达时连接已不在、事件循环拆除时命令还没执行</param>
/// <returns>ERR_OK 命令已入队;fd 为 INVALID_SOCK 时返回 ERR_FAILED 并已用 fcb 回收 extra，
/// 调用方不可再释放。命令入队恒成功,ev_free 已启动时同样返 ERR_OK</returns>
int32_t ev_ud_context(ev_ctx *ctx, sock_ctx *sk, void *extra, free_cb fcb);
/// <summary>
/// 投一个回调到第 index 个 event 线程上执行：在该线程本轮事件派发完之后跑，回调里 ev_send 的数据随后冲出。
/// 在该线程上调用也只排队不当场执行，回调永远不嵌在 socket 回调里；回调里再投的留到下一轮。
/// 不检查 ev_free 是否已开始，ev_free 返回之后再调是释放后使用
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="index">event 线程下标 [0, nthreads)，越界断言</param>
/// <param name="cb">回调，必须非 NULL</param>
/// <param name="fcb">cb 没机会跑(ev_free 时还在队里)就用它释放 arg：在 ev_free 的调用线程上调，
///   那时 event 线程已停，里面不得再调 ev_*；可为 NULL，arg 为 NULL 时也不调</param>
/// <param name="arg">透传给 cb / fcb</param>
void ev_defer_exec(ev_ctx *ctx, int32_t index, defer_exec_cb cb, free_cb fcb, void *arg);
/// <summary>
/// 当前线程是 ctx 的第几个 event 线程，每次现读
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <returns>event 线程下标；不在 ctx 的 event 线程上(含别的 ev_ctx 的线程)返回 INVALID_INDEX</returns>
int32_t ev_cur_index(ev_ctx *ctx);

#endif//EVENT_H_
