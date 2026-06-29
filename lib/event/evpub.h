#ifndef EVPUB_H_
#define EVPUB_H_

#include "utils/utils.h"
#include "containers/queue.h"
#include "containers/sarray.h"
#include "thread/spinlock.h"
#include "utils/buffer.h"
#include "utils/netaddr.h"
#include "base/structs.h"

#define GET_POS(fd, n) ((fd) % (n))// 根据fd计算索引位置
#define GET_PTR(p, n, fd) (1 == (n) ? (p) : &(p)[GET_POS((fd), (n))])// 根据fd获取对应的指针

struct evssl_ctx;
struct watcher_ctx;
struct sock_ctx;
struct listener_ctx;
// socket 状态标志位
typedef enum sock_status {
    STATUS_NONE = 0x00,         // 无状态
    STATUS_SENDING = 0x01,      // 正在发送数据
    STATUS_NORECV = 0x02,       // 仅 IOCP：KeyUpdate 探针期暂停收(ol_r 未重投 WSARecv)
    STATUS_ERROR = 0x04,        // 发生错误
    STATUS_REMOVE = 0x08,       // 待移除
    STATUS_CLIENT = 0x10,       // 作为客户端
    STATUS_SSLEXCHANGE = 0x20,  // 是否切换成SSL链接，发送队列为空时移除该标识，并开始SSL握手
    STATUS_AUTHSSL = 0x40,      // SSL握手中
    STATUS_KEYUPDATE = 0x80,    // 数据期 TLS1.3 KeyUpdate 等写就绪(两平台均仅数据期)；Unix 注册 EVENT_WRITE，IOCP 投 0 字节 WSASend 探针
    STATUS_GRACEFUL_CLOSE = 0x100// ev_close(immed=0) 标记，buf_s 发完后 _close_tcp
}sock_status;
// UDP 多播 setsockopt 操作类型,由 ev_udp_join/leave/ttl/loop 投递 CMD_UDP_OPT 时填写
typedef enum udp_opt_type {
    UDP_OPT_JOIN = 0x01,   // 加入多播组(IP_ADD_MEMBERSHIP / IPV6_JOIN_GROUP)
    UDP_OPT_LEAVE,         // 离开多播组(IP_DROP_MEMBERSHIP / IPV6_LEAVE_GROUP)
    UDP_OPT_TTL,           // 多播 TTL(IP_MULTICAST_TTL / IPV6_MULTICAST_HOPS)
    UDP_OPT_LOOP           // 多播本机回环(IP_MULTICAST_LOOP / IPV6_MULTICAST_LOOP)
}udp_opt_type;
// CMD_UDP_OPT 命令携带的参数：业务侧 MALLOC,事件线程内 setsockopt 后 FREE。
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
// 网络事件上下文
typedef struct ev_ctx {
    uint32_t nthreads;              // 工作线程数
#ifdef EV_IOCP
    uint32_t nacpex;                // AcceptEx线程数
    struct acceptex_ctx *acpex;     // AcceptEx上下文数组
#endif
    struct watcher_ctx *watcher;    // 事件监听器数组
    array_ctx arrlsn;               // 监听器列表（元素 listener_ctx *）
    spin_ctx spin;                  // 保护arrlsn的自旋锁
}ev_ctx;

// 回调函数类型定义
// accept_cb/connect_cb 返回失败则自动关闭链接，并触发close_cb回调
// 启用 ssl 握手未完成前（ssl_exchanged_cb）不能调用发送，否则会关闭链接
// ssl_exchanged_cb 返回失败则自动关闭链接
typedef int32_t(*accept_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid,
                            ud_cxt *ud);// 接受新连接回调
typedef int32_t(*connect_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid,
                             int32_t err, ud_cxt *ud);// 连接完成回调
typedef int32_t(*ssl_exchanged_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid,
                                   int32_t client, ud_cxt *ud, void *ssl);// SSL握手完成回调（ssl 为 SSL 对象指针，WITH_SSL 时有效，否则为 NULL）
typedef void(*recv_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid,
                      int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud);// 接收数据回调
typedef void(*send_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid,
                       int32_t client, size_t size, ud_cxt *ud);// 发送完成回调
typedef void(*close_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid,
                        int32_t client, ud_cxt *ud);// 连接关闭回调
typedef void(*recvfrom_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid,
                           char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud);// UDP接收回调
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
typedef struct skpool_args {
    SOCKET fd;
    cbs_ctx *cbs;
    ud_cxt *ud;
}skpool_args;

// fd → sock_ctx hashmap 工具集
// hashmap哈希函数：以fd作为key计算哈希值（hashmap_new_with_allocator 回调）
uint64_t _evpub_sockel_hash(const void *item, uint64_t seed0, uint64_t seed1);
// hashmap比较函数：比较两个sock_ctx的fd（hashmap_new_with_allocator 回调）
int _evpub_sockel_compare(const void *a, const void *b, void *ud);
// 根据fd从watcher的hashmap查找sock_ctx
struct sock_ctx *_evpub_sockel_get(struct watcher_ctx *watcher, SOCKET fd);
// 将sock_ctx加入watcher的hashmap（断言不重复）
void _evpub_sockel_add(struct watcher_ctx *watcher, struct sock_ctx *skctx);
// 从watcher的hashmap中移除fd，返回 hashmap spare 缓冲指针（下次操作前有效，调用方按需用）
void *_evpub_sockel_remove(struct watcher_ctx *watcher, SOCKET fd);

//sock_ctx 池相关
void *_evpub_sk_new(void *args);
void _evpub_sk_free(void *sk);
void _evpub_sk_clear(void *sk);
void _evpub_sk_reset(void *sk, void *args);

// 统一释放一个 off_buf_ctx：shared==NULL 走独占 FREE(data)；非 NULL 走多播 ref-- 路径
void _evpub_off_buf_release(off_buf_ctx *buf);
// 以下为模块内部公共函数
// 清空发送缓冲队列并释放数据
void _evpub_off_buf_clear(queue_ctx *bufs);
// 清空 UDP 发送队列(sendto_ctx)并释放各 payload
void _evpub_sendto_clear(queue_ctx *bufs);
// 设置socket选项：无延迟 + 非阻塞
int32_t _evpub_nodelay_nonblock(SOCKET fd);
// 创建socket（SOCK_DGRAM/SOCK_STREAM，AF_INET/AF_INET6）
SOCKET _evpub_create_sock(int32_t type, int32_t family);
// 创建并绑定监听socket
SOCKET _evpub_listen(netaddr_ctx *addr);
// 创建并绑定UDP socket
SOCKET _evpub_udp(netaddr_ctx *addr);
// 从socket读取数据（支持SSL/普通）
int32_t _evpub_sock_read(SOCKET fd, IOV_TYPE *iov, uint32_t niov, void *arg, size_t *readed);
// 向socket发送数据（支持SSL/普通）
int32_t _evpub_sock_send(SOCKET fd, queue_ctx *buf_s, size_t *nsend, void *arg);
// 设置ud_cxt的子协议extra字段
void _evpub_set_secextra(ud_cxt *ud, void *val);

#endif//EVPUB_H_
