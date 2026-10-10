#ifndef EVPUB_H_
#define EVPUB_H_

#include "utils/utils.h"
#include "containers/queue.h"
#include "containers/sarray.h"
#include "thread/spinlock.h"
#include "utils/buffer.h"
#include "utils/netaddr.h"
#include "utils/tda.h"
#include "base/structs.h"
#include "containers/slist.h"

#define EVENT_WAIT_TIMEOUT   100 // 事件循环等待超时（毫秒）
#define EVENT_CHANGES_CNT    128 // 事件变更队列初始容量
#define EVENT_CHECK_INTERVAL 5 // 每隔多少次事件循环才检查一次定时器，避免每次紧循环都调用 clock_gettime
#define INIT_EVENTS_CNT      256 // 初始事件槽位数量
#define INIT_SENDBUF_LEN     32 // 发送缓冲区初始长度
#define QUEUE_OVERLOAD_RATIO 3 // 队列积压告警初始阈值 = 容量 / RATIO，触发后翻倍，空队列重置
#define WB_WARN_INIT_SIZE   (1024 * 1024) // 单 sock 发送缓冲字节告警首阈值；触发后翻倍（1MB→2MB→4MB...），队列清空后复位；0 表示禁用
#define EVENT_TICK_MIN       10// event 线程周期驱动(ev_tick)的最小间隔(毫秒),防 tick 返回 0 忙轮询
#define ACCEPT_BACKOFF_MS    500// accept 遇 EMFILE/ENFILE 后暂停监听、退避重试的间隔(毫秒)
#define UDP_RECV_MAX_ERRS    3// 单次唤醒内 recvmsg 连续失败上限；超限认定 fd 异常转关闭，防不消耗 datagram 的错误原地打转
#define CLOSE_LINGER_MS      3000// 本端主动关闭后继续读掉对端数据的最长时间(毫秒，机制见 STATUS_LINGER)；0 表示不延迟关闭
#define CLOSE_LINGER_BYTES   (1024 * 1024)// 延迟关闭期间最多读掉的字节数，超过就直接关 fd；它不是开关，取 0 即对端再发一个字节就关
#define EVTASK_SLOW_MS       20// 绑到 net 线程上的 task(task_bind_net)一轮跑超过这么多毫秒就告警(同线程所有连接都在等它)，之后按翻倍阈值；0 关闭

// accept 出的连接继承监听 socket 上的 TCP_NODELAY 与保活参数，非 0 的平台都先在监听 socket 上设一次：
// 1 = 全部继承(Linux / FreeBSD / Windows 实测)；2 = 只有保活空闲时长不继承、accept 后补这一项(macOS 实测)；
// 0 = 没验证过，仍逐连接全设
#if defined(OS_LINUX) || defined(OS_FBSD) || defined(OS_WIN)
    #define ACCEPT_INHERIT_OPTS 1
#elif defined(OS_DARWIN)
    #define ACCEPT_INHERIT_OPTS 2
#else
    #define ACCEPT_INHERIT_OPTS 0
#endif
// accept 出的 fd 继承监听 fd 的非阻塞(macOS 实测；Linux 不继承，BSD 走 accept4 用不上)，不用再设一次。
// 前提是监听 fd 恒由 _evpub_listen 建成非阻塞
#if defined(OS_DARWIN)
    #define ACCEPT_INHERIT_NONBLOCK 1
#else
    #define ACCEPT_INHERIT_NONBLOCK 0
#endif
#if defined(OS_WIN)
// Windows SOCKET 句柄恒为 4 的倍数(低 2 位保留),fd%n 在偶数 n 下残值聚集(n=4 全落 watcher 0)致 IOCP 多线程退化;
// 先右移 2 位消除恒零低位再取模,恢复均匀分布
#define CALC_WATCHER_INDEX(fd, n) (((fd) >> 2) % (n))
#else
#define CALC_WATCHER_INDEX(fd, n) ((fd) % (n))// 由 fd 算所属 watcher 下标,即 sock_ctx.index 的取值
#endif// OS_WIN
// 取 tcp 结构上的 SSL 对象；未编 SSL 时恒 NULL。收 ssl 的那几个函数形参都是 void *，
// 本就不跟着 #if WITH_SSL 切（见 _evpub_sock_send / _evpub_close_flush_tcp 的说明），
// 调用点也不该各套一层：unix 与 IOCP 两侧共 8 处，只差这一个实参
#if WITH_SSL
#define TCP_SSL(t) ((t)->ssl)
#else
#define TCP_SSL(t) NULL
#endif// WITH_SSL
// 只在手上还没有 sock_ctx(刚 accept 出裸 fd)时用来定归属;已有 sock_ctx 的一律读 sk->index,
// 别再按 fd 重算——listener 与 pipe 的 index 本来就不等于 CALC_WATCHER_INDEX(fd)
#define CALC_WATCHER(p, n, fd) (1 == (n) ? (p) : &(p)[CALC_WATCHER_INDEX((fd), (n))])
// 回调与消息里的 client 形参恒取 0/1。BIT_CHECK 拿到的是 STATUS_CLIENT 的原值、不是 1，必须在这里归一化——
// 上层文档都按 1 写，透传原值会让 == 1 的判定永远不成立
#define SOCK_IS_CLIENT(status) (BIT_CHECK((status), STATUS_CLIENT) ? 1 : 0)
// fd → evsock_ctx 哈希表的 HASHFN / CMPFN。表由 uev.h / iocp.h 各自 HASHMAP_DECL 出来
// (evsock_ctx 是平台各自定义的),宏到那时才展开,所以这里 evsock_ctx 不完整也没关系。
// 比较不用相减:SOCKET 在 Win64 是 UINT_PTR,差值会截断成 int 溢出
#define SOCKEL_HASH(e) hash_u64((uint64_t)(*(e))->sk.fd)
#define SOCKEL_CMP(a, b) (((*(a))->sk.fd < (*(b))->sk.fd) ? -1 : ((*(a))->sk.fd > (*(b))->sk.fd) ? 1 : 0)

struct evssl_ctx;
struct watcher_ctx;
struct evsock_ctx;
struct listener_ctx;
struct timer_ctx;
// socket 状态标志位
typedef enum sock_status {
    STATUS_NONE = 0x00,         // 无状态
    // 正在发送数据。IOCP 指 WSASend 在途(跨完成回调)，攒发链同步发送期间(含 s_cb)也置位；
    // uev 只覆盖 s_cb 执行期。同步发送期间置位都是为了挡回调里的同步重入
    STATUS_SENDING = 0x01,
    STATUS_ERROR = 0x02,        // 发生错误
    // 待移除，关闭要等手上的事做完：IOCP 等在途的 WSASend(TCP) / WSASendTo(UDP) 完成，Unix 等 STATUS_INCB 的回调返回
    STATUS_REMOVE = 0x04,
    // 仅 Unix：本连接的某个回调(acp / conn / exch / r / s)正在执行。期间要关它(同线程发送失败、ev_close
    // 重注册失败)只记 REMOVE，回调返回再关——回调里还在用它的状态与收包缓冲，CLOSE 也得排在回调推出的消息之后。
    // IOCP 本就不在回调里关
    STATUS_INCB = 0x08,
    STATUS_CLIENT = 0x10,       // 作为客户端
    // 数据已入 buf_s 但故意没发，等本轮派发结束后一次合并发出；两个后端都用，IOCP 只用于明文。
    // 位与在 watcher->flushes 上一一对应：置清位在各自的 flush_link / flush_unlink，
    // 唯独 Unix 轮末冲刷(_uev_flush_pending)摘链时就地清，不经 flush_unlink
    STATUS_FLUSHPEND = 0x20,
    STATUS_ESTABLISHED = 0x40,     // TCP 已连通：accept 出来即置，connect 在完成回调里确认成败后置，连接失败收尾时撤掉
    // 本端主动关闭已连通的连接时不关读：先关写发 FIN，之后读到的一律丢掉，等对端 FIN、到 CLOSE_LINGER_MS
    // 或丢满 CLOSE_LINGER_BYTES 才关 fd。关读或带着没读的数据关 fd 都会让内核回 RST，Windows 收到 RST
    // 会把已到但没读的响应一起丢掉。关闭回调、ud 清理都在进入延迟关闭前做完，对上层不可见
    STATUS_LINGER = 0x80,          // 关闭时置位：关 fd 之前要延迟关闭
    STATUS_LINGERING = 0x100,      // 延迟关闭中；与"在 watcher->lingers 上"一一对应
    // 下面三个记"连接是怎么断的"，只由收发失败路径置位（_evpub_mark_close）；
    // 都没置即本地主动关闭，故 ev_close / task 拆除等路径无需标记
    STATUS_PEER_FIN = 0x200,       // 对端有序结束发送方向：裸 TCP 收到 FIN，SSL 收到 close_notify
    STATUS_PEER_ABORT = 0x400,     // 收发失败：RST、读写错误、SSL 协议错
    // 以下 SSL 专用；新增非 SSL 位加在 #if 上面，别插进来
#if WITH_SSL
    STATUS_PEER_TRUNCATED = 0x800, // TLS 没发 close_notify 就断了，收全与被截断分不出
    STATUS_SSLEXCHANGE = 0x1000, // 是否切换成SSL链接，发送队列为空时移除该标识，并开始SSL握手
    STATUS_AUTHSSL = 0x2000,    // SSL握手中
#ifdef EV_IOCP
    STATUS_NORECV = 0x4000,     // 仅 IOCP：KeyUpdate 探针期暂停收(ol_r 未重投 WSARecv)
#endif
    // 下面三个是数据期 TLS1.3 的读写互卡。前两个的后缀表示"在等哪一边就绪"，
    // 第三个表示"我这边还有没发完的应用写"。后两个互斥，由 _usk_tcp_send /
    // _olp_tcp_send 单点保证；KEYUPDATE_WRITE 可与 KEYUPDATE_READ 共存，它与
    // WPEND_SSL 不共存只是各发送入口那道 !KEYUPDATE_WRITE 守卫撑出来的，别删
    STATUS_KEYUPDATE_WRITE = 0x8000,// 读的时候 SSL 说要先写：Unix 注册 EVENT_WRITE，IOCP 投 0 字节 WSASend 探针
    STATUS_KEYUPDATE_READ = 0x10000, // 发的时候 SSL 说要先读到对端数据，挂着等读就绪再重试发送：
                                     // Unix 摘掉 EVENT_WRITE 只留 EVENT_READ，IOCP 交还 SENDING 不投探针。
                                     // 置位来自对端把 post-handshake 消息(现实中是 NewSessionTicket)
                                     // 拆到多条 TLS 记录、后一条未到；KeyUpdate 本身触发不了。
                                     // 前提由 test_event.c 的 test_ssl_write_wants_read 钉着，别当死码删
    STATUS_WPEND_SSL = 0x20000       // 发的时候撞上 socket 满(WANT_WRITE)：OpenSSL 把那条应用记录挂着，
                                     // 此期间不得调 SSL_read——它处理对端 KeyUpdate 要回发的握手记录更短，
                                     // 走同一个 ssl3_write_bytes，会撞上"重试长度不得小于挂起量"那道守卫，
                                     // 整条连接被判死。置清位都只在 wpend_link/unlink：
                                     // 发送排空、改挂 KEYUPDATE_READ、连接关闭、看门狗到期都会清
#endif
}sock_status;
// UDP 多播 setsockopt 操作类型,由 ev_udp_join/leave/ttl/loop 经 ev_props 投递时填写
typedef enum udp_opt_type {
    UDP_OPT_JOIN = 0x01,   // 加入多播组(IP_ADD_MEMBERSHIP / IPV6_JOIN_GROUP)
    UDP_OPT_LEAVE,         // 离开多播组(IP_DROP_MEMBERSHIP / IPV6_LEAVE_GROUP)
    UDP_OPT_TTL,           // 多播 TTL(IP_MULTICAST_TTL / IPV6_MULTICAST_HOPS)
    UDP_OPT_LOOP           // 多播本机回环(IP_MULTICAST_LOOP / IPV6_MULTICAST_LOOP)
}udp_opt_type;
// ev_props 携带的 UDP 多播 setsockopt 参数：业务侧 MALLOC,事件线程内 setsockopt 后 FREE。
// 字段按 op 不同使用：JOIN/LEAVE 用 group_ip+iface_str；TTL 用 ttl；LOOP 用 loop
typedef struct udp_opt_arg {
    uint8_t  ttl;               // op=UDP_OPT_TTL 时使用;1=仅本网段,255=跨广域
    int32_t  loop;              // op=UDP_OPT_LOOP 时使用;0/1
    udp_opt_type op;            // 操作类型
    char group_ip[64];          // op=JOIN/LEAVE 时使用,多播组地址字符串(支持 IPv4/IPv6)
    char iface_str[64];         // op=JOIN/LEAVE 时使用,IPv4 走 IP 字符串,IPv6 走接口名(如 "en0");空串走系统默认
}udp_opt_arg;
// UDP 发送队列元素：地址与 payload 分离,copy=0 时 data 直接复用调用方缓冲(零拷贝)
typedef struct sendto_ctx {
    size_t len;        // payload 长度
    void *data;        // payload 指针(copy=1 时为内部 MALLOC,copy=0 时为调用方转移所有权)
    netaddr_ctx addr;  // 目标地址
}sendto_ctx;
QUE_DECL(obuf_que, off_buf_ctx)// TCP 发送队列
QUE_DECL(sbuf_que, sendto_ctx)// UDP 发送队列
typedef struct recvfrom_ctx {
    size_t len;
    netaddr_ctx addr;  // 发送端地址
    char data[];
}recvfrom_ctx;
typedef void(*defer_exec_cb)(void *arg);// ev_defer_exec 投递的回调
// ev_defer_exec 的投递项：回调、它没机会跑时释放 arg 的 fcb、透传参数
typedef struct defer_exec_item {
    defer_exec_cb cb;
    free_cb fcb;
    void *arg;
}defer_exec_item;
QUE_DECL(defer_exec_que, defer_exec_item)// 推迟执行队列：只在所属 event 线程上进出
ARR_DECL(lsn_arr, struct listener_ctx *)
// 网络事件上下文
typedef struct ev_ctx {
    uint32_t nthreads;              // 工作线程数
    atomic_t stopping;              // ev_free 入口即置 1：此后拒绝新建 listener/连接/UDP。不能用 watcher->stop 判断——那个由 CMD_STOP 到达后 event 线程异步置位，ev_free 刚进来时仍为 0，据它检查会漏
#ifdef EV_IOCP
    uint32_t nacpex;                // AcceptEx线程数
    atomic_t nlsn;                  // 存活listener计数（ev_free关闭阶段排空同步用）
    atomic_t ndead_total;           // 所有listener的dead槽总数（==sum(lsn->ndead)）；==0时_olp_revive_dead免锁免遍历
    struct acceptex_ctx *acpex;     // AcceptEx上下文数组
#endif
    struct watcher_ctx *watcher;    // 事件监听器数组
    lsn_arr arrlsn;                 // 监听器列表
    spin_ctx spin;                  // 保护arrlsn的自旋锁
}ev_ctx;

// 回调函数类型定义
// accept_cb/connect_cb 返回失败则自动关闭链接，并触发close_cb回调
// 启用 ssl 握手未完成前（ssl_exchanged_cb）不能调用发送，否则会关闭链接
// ssl_exchanged_cb 返回失败则自动关闭链接
typedef int32_t(*accept_cb)(ev_ctx *ev, sock_ctx *sk, ud_cxt *ud);// 接受新连接回调
typedef int32_t(*connect_cb)(ev_ctx *ev, sock_ctx *sk, int32_t err, ud_cxt *ud);// 连接完成回调
typedef int32_t(*ssl_exchanged_cb)(ev_ctx *ev, sock_ctx *sk, int32_t client,
                                   ud_cxt *ud, void *ssl);// SSL握手完成回调（ssl 为 SSL 对象指针，WITH_SSL 时有效，否则为 NULL）
typedef void(*recv_cb)(ev_ctx *ev, sock_ctx *sk, int32_t client,
                       buffer_ctx *buf, size_t size, ud_cxt *ud);// 接收数据回调
typedef void(*send_cb)(ev_ctx *ev, sock_ctx *sk, int32_t client,
                       size_t size, ud_cxt *ud);// 发送完成回调
typedef void(*close_cb)(ev_ctx *ev, sock_ctx *sk, int32_t client,
                        int32_t erro, ud_cxt *ud);// 连接关闭回调（erro 为 close_type）
typedef void(*recvfrom_cb)(ev_ctx *ev, sock_ctx *sk,
                           char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud);// UDP接收回调
typedef int32_t(*props_cb)(struct watcher_ctx *watcher, struct evsock_ctx *evsk,
                           void *data, uint64_t number);// 返回值为0 不执行free。
// 回调函数集合
typedef struct cbs_ctx {
    accept_cb acp_cb;       // 接受连接回调
    connect_cb conn_cb;     // 连接完成回调
    ssl_exchanged_cb exch_cb; // SSL握手完成回调
    recv_cb r_cb;           // 接收数据回调
    send_cb s_cb;           // 发送完成回调
    close_cb c_cb;          // 连接关闭回调
    recvfrom_cb rf_cb;      // UDP接收回调
    free_cb ud_free;        // 用户数据释放回调
}cbs_ctx;
// 对象池构造/复位的入参。sk 只带 fd 与 index 两个值,skid 传 0 表示待分配——
// _evpub_sk_new / _evpub_sk_reset 一律用 createid() 覆写它。整体传 sock_ctx 而非摊开字段,
// 是为了以后 sock_ctx 再加字段时这几个构造函数不用跟着改
typedef struct skpool_args {
    sock_ctx sk;
    cbs_ctx *cbs;
    ud_cxt *ud;
}skpool_args;
// event 线程周期驱动回调:返回"距下次应被驱动的毫秒数",event 循环取所有 tick 的最小值作等待超时
typedef uint32_t (*ev_tick_cb)(void *ud, uint64_t now_ms);
// 周期驱动节点(侵入式,挂 watcher->ticks);由需周期驱动的模块(如 kcp)注册
typedef struct ev_tick {
    list_node node;// 挂 watcher->ticks 链表
    ev_tick_cb cb; // 驱动回调,返回距下次的毫秒数
    void *ud;      // 透传给 cb 的上下文
}ev_tick;

// 新建 socket 归哪个 watcher：调用方指定了就用它(越界断言)，INVALID_INDEX 按 fd 分配
static inline int32_t _evpub_launch_index(ev_ctx *ctx, SOCKET fd, int32_t index) {
    if (INVALID_INDEX == index) {
        return (int32_t)CALC_WATCHER_INDEX(fd, ctx->nthreads);
    }
    ASSERTAB(index >= 0 && index < (int32_t)ctx->nthreads, "watcher index out of range.");
    return index;
}
// 登记/注销本线程正在跑的 watcher 事件循环，须在循环入口与出口各调一次
void _evpub_set_cur_watcher(struct watcher_ctx *watcher);
// 调用方是否就在该 watcher 的事件线程上；是则命令可就地执行，不必入队
int32_t _evpub_inloop(struct watcher_ctx *watcher);
// fd → evsock_ctx 哈希表(sockel_map)工具集
// 根据fd从watcher的哈希表查找evsock_ctx
struct evsock_ctx *_evpub_sockel_get(struct watcher_ctx *watcher, SOCKET fd);
// 将evsock_ctx加入watcher的哈希表（断言不重复）
void _evpub_sockel_add(struct watcher_ctx *watcher, struct evsock_ctx *evsk);
// 从watcher的哈希表中移除fd，返回表内 spare 缓冲指针（下次操作前有效，调用方按需用）
void *_evpub_sockel_remove(struct watcher_ctx *watcher, SOCKET fd);
int32_t _evpub_checkid(struct evsock_ctx *evsk, const uint64_t skid);
// 获取ud_cxt
ud_cxt *_evpub_get_ud(struct evsock_ctx *evsk);
// 注册周期驱动节点到 watcher->ticks(须在该 fd 所属 event 线程内调用)
void _evpub_tick_add(struct watcher_ctx *watcher, ev_tick *tk);
// 从 watcher->ticks 注销周期驱动节点(须在该 fd 所属 event 线程内调用)
void _evpub_tick_remove(struct watcher_ctx *watcher, ev_tick *tk);
// 按"cb 非 NULL 即已挂"的口径挂/摘周期驱动节点，须在该 watcher 的 event 线程内调用。
// attach 已挂则不动，否则记下 cb/ud 并挂进 watcher->ticks；detach 未挂则不动，否则摘出并把 cb 置 NULL
void _evpub_tick_attach(struct watcher_ctx *watcher, ev_tick *tk, ev_tick_cb cb, void *ud);
void _evpub_tick_detach(struct watcher_ctx *watcher, ev_tick *tk);
// 驱动 watcher->ticks 全部节点:逐个调用 tick 回调,返回下轮 wait 超时(ms,clamp 到 [EVENT_TICK_MIN, EVENT_WAIT_TIMEOUT]);
// *now_ms 回填本轮时钟,无 tick 时置 0(供调用方 drain/shrink 复用,省一次 timer_cur_ms)
uint32_t _evpub_tick_drive(struct watcher_ctx *watcher, struct timer_ctx *timer, uint64_t *now_ms);
// 跑投递队列里进来时已有的项，回调里新投的留到下一轮；返回跑了几项。只在该 watcher 的 event 线程上调
uint32_t _evpub_defer_exec_drain(struct watcher_ctx *watcher);
// 投递队列里还有没跑的项
int32_t _evpub_defer_exec_pending(struct watcher_ctx *watcher);
// 释放投递队列，没跑成的逐项调 fcb；event 线程停下之后调
void _evpub_defer_exec_free(struct watcher_ctx *watcher);
// 该 watcher的 计时器
struct timer_ctx *_evpub_watcher_timer(struct watcher_ctx *watcher);
// 获取 evsock_ctx 的 socket 类型（SOCK_STREAM/SOCK_DGRAM），供不知道 evsock_ctx 完整定义的调用方使用
int32_t _evpub_sock_type(struct evsock_ctx *evsk);

//evsock_ctx 池相关
void *_evpub_sk_new(void *args);
void _evpub_sk_free(void *sk);
void _evpub_sk_clear(void *sk);
void _evpub_sk_reset(void *sk, void *args);
// 定期收缩对象池（调用方按周期节流触发；now_ms 距上次不足 SHRINK_TIME 则跳过）
void _evpub_pool_shrink(struct watcher_ctx *watcher, uint64_t *shrink_start, uint64_t now_ms);

void _evpub_share_data_free(void *arg);
// 统一释放一个 off_buf_ctx：shared==NULL 走独占 FREE(data)；非 NULL 走多播 ref-- 路径
void _evpub_off_buf_release(off_buf_ctx *buf);
// 以下为模块内部公共函数
// 清空发送缓冲队列并释放数据
void _evpub_off_buf_clear(obuf_que *bufs);
// 清空 UDP 发送队列(sendto_ctx)并释放各 payload
void _evpub_sendto_clear(sbuf_que *bufs);
// TCP 发送队列准入(未建连 / SSL 握手期 / 队列超上限)：通过返 1；拒收返 0 且已落 WARN，调用方丢数据并断连。
// "已在关闭流程"那道门动作不同(只丢不断)，留在调用点
int32_t _evpub_recvbuf_full(buffer_ctx *buf_r, SOCKET fd);
int32_t _evpub_sendqu_check_tcp(uint32_t nqu, int32_t status, SOCKET fd);
// UDP 发送队列准入：仅判队列超上限。通过返 1；拒收返 0 且已落 WARN，调用方丢包不断连
int32_t _evpub_sendqu_check_udp(uint32_t nqu, SOCKET fd);
// 入队字节累计的增长告警(tda 翻倍阈值)；istcp 只用于挑 TCP / UDP 两条文案
void _evpub_sendqu_tda(tda_ctx *tda, size_t wb_size, SOCKET fd, int32_t istcp);
// 关闭前把 send queue 冲一次：能写进内核的(关闭帧、COM_QUIT 这类小控制包)送达，写不进去的
// 连同连接一起丢并落 WARN。不留"等发完再关"的中间态——那个态没有上限，对端不读就永久占住 fd。
// KeyUpdate 挂着 SSL_read(理由见 _uev_add_bufs_send)时发不得，只丢不冲。
// 冲出去的字节不报 MSG_TYPE_SEND：调用方此刻尚未置 STATUS_ERROR，回调进来即重入。
// ssl 收 void * 而非 SSL *：明文路径也走这里，不跟着 #if WITH_SSL 一起切
void _evpub_close_flush_tcp(SOCKET fd, obuf_que *buf_s, int32_t status, size_t *wb_size, void *ssl);
// 就地拆连接，两平台各走自己的断连实现。给协议层的命令回调用：命令通道只报成功/失败，
// 没有 unpack 路径上 PROT_ERROR 那条断链通道，撞上必须断连的误用时只能由它来关
void _evpub_disconnect(struct watcher_ctx *watcher, struct evsock_ctx *evsk);
#if WITH_SSL
// ssl_exchange 的准入门 + CLIENT 位落定，两平台逐字相同的那一段。通过返 1 且 CLIENT 位已按 client 落定；
// 拒收返 0，该告警的已落 WARN。
// "不是 SOCK_STREAM" 那道门不在此处：它是调用方 UPCAST 成 tcp 结构的前提，进来晚了就已经越界读了。
// 收 const void * 而非 SSL *：evpub.h 不引 openssl 头
int32_t _evpub_ssl_exchange_check(const void *ssl, int32_t *status, int32_t client);
#endif
// 设保活。Windows 下它是 SIO_KEEPALIVE_VALS 这个 IOCTL，对未 bind 未连接的 socket 没有意义，
// 故 IOCP 侧外连要等连通后才设；Unix 上是普通 socket 选项，ev_connect 在 connect 前就跟 nodelay 一道设了
int32_t _evpub_tcp_keepalive(SOCKET fd);
// accept 出的连接补设 TCP_NODELAY 与保活；ACCEPT_INHERIT_OPTS 取 1 直接返回，取 2 只补保活空闲时长
int32_t _evpub_accept_opts(SOCKET fd);
// ev_connect / ev_listen / ev_udp 的公共前导：校验回调、拒绝 ev_free 期间的调用、解析地址。
// 失败时调用方直接 return ERR_FAILED，不要再碰 ud：ud 已被 UD_FREE，唯一例外是 cbs 本身为 NULL
// （ud_free 就挂在 cbs 里，无从释放）。只有地址解析失败那条会落日志，前两条静默
int32_t _evpub_sock_launch_check(ev_ctx *ctx, const char *ip, uint16_t port, cbs_ctx *cbs,
                                 ud_cxt *ud, int32_t isudp, netaddr_ctx *addr);
// 创建并绑定监听socket
SOCKET _evpub_listen(netaddr_ctx *addr);
// 创建并绑定UDP socket
SOCKET _evpub_udp(netaddr_ctx *addr);
// 从socket读取数据（支持SSL/普通）；返回 1 表示对端有序关闭——裸 socket 读到 FIN，
// SSL 收到 close_notify；返回 2 表示 TLS 没发 close_notify 就断了。
// 取正数,这样只认 ERR_OK 的调用方仍按失败处理,漏改一处不会静默死循环
int32_t _evpub_sock_read(SOCKET fd, IOV_TYPE *iov, uint32_t niov, void *arg, size_t *readed);
// 记下这次收发失败是"对端有序结束"、"TLS 无 close_notify 断开"还是"异常中断"，供关闭回调回带 close_type。
// rtn 传本次收发的返回码，传输层自身报错时传 ERR_FAILED
void _evpub_mark_close(int32_t *status, int32_t rtn);
// 由 STATUS_PEER_* 得出 close_type，三个位都没置即本地主动关闭
int32_t _evpub_close_type(int32_t status);
// 本端主动关闭时要不要延迟关闭(机制见 STATUS_LINGER)：已连通、对端还没表态结束、且功能没关
int32_t _evpub_linger_want(int32_t status);
// 延迟关闭期间把 fd 上已到的数据读掉丢弃，*bytes 按次累加。返回 0 表示读空了接着等；
// 返回 1 表示该收尾了：对端 FIN、读错、或累计丢弃超过 CLOSE_LINGER_BYTES。
// buf/lens 只当读的落脚处，内容随即丢掉，越大系统调用越少
int32_t _evpub_linger_drain(SOCKET fd, char *buf, size_t lens, size_t *bytes);
// 向socket发送数据（支持SSL/普通）；返回 1 / 2 的含义与取正数的理由同 _evpub_sock_read，
// 只是触发点在发送方向先读到对端记录时（仅 SSL 路径，明文路径只返 ERR_OK / ERR_FAILED）
int32_t _evpub_sock_send(SOCKET fd, obuf_que *buf_s, size_t *nsend, void *arg);
// UDP 发送缓冲入队并尝试立即发送（IOCP/uev 平台无关封装）；
// tried 非 0 表示调用方在入队前已经尝试过一次发送（如 _evpub_try_sendto 遇到 EAGAIN），
// 此次必然复现，跳过重复尝试，仅确保写事件已注册（IOCP 平台忽略该参数）
void _evpub_add_bufs_sendto(struct watcher_ctx *watcher, struct evsock_ctx *evsk, sendto_ctx *buf, int32_t tried);
// 尝试直接发送 UDP 数据，不转移 data 所有权（调用方返回后可自由处置该内存）；
// 返回 0 表示已处理完(发送成功或致命错误已断开)，调用方无需任何后续操作；
// 返回 1 表示需要调用方继续(发送队列已有积压、EAGAIN，或 IOCP 侧另有在途 IRP)，
// 自行 MALLOC+memcpy 后以 tried=1 转入 _evpub_add_bufs_sendto 排队
int32_t _evpub_try_sendto(struct watcher_ctx *watcher, struct evsock_ctx *evsk, const void *data, size_t len, netaddr_ctx *addr);

#endif//EVPUB_H_
