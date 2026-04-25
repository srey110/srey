#ifndef EVPUB_H_
#define EVPUB_H_

#include "utils/utils.h"
#include "containers/queue.h"
#include "containers/sarray.h"
#include "thread/spinlock.h"
#include "utils/buffer.h"
#include "utils/netaddr.h"
#include "base/structs.h"

// socket 状态标志位
typedef enum sock_status {
    STATUS_NONE = 0x00,         // 无状态
    STATUS_SENDING = 0x01,      // 正在发送数据
    STATUS_ERROR = 0x02,        // 发生错误
    STATUS_REMOVE = 0x04,       // 待移除
    STATUS_CLIENT = 0x08,       // 作为客户端
    STATUS_SSLEXCHANGE = 0x10,  // 是否切换成SSL链接，发送队列为空时移除该标识，并开始SSL握手
    STATUS_AUTHSSL = 0x20       // SSL握手中
}sock_status;
// 网络事件上下文
typedef struct ev_ctx {
    uint32_t nthreads;              // 工作线程数
#ifdef EV_IOCP
    uint32_t nacpex;                // AcceptEx线程数
    struct acceptex_ctx *acpex;     // AcceptEx上下文数组
#endif
    struct watcher_ctx *watcher;    // 事件监听器数组
    arr_ptr_ctx arrlsn;             // 监听器列表
    spin_ctx spin;                  // 保护arrlsn的自旋锁
}ev_ctx;
struct evssl_ctx;

// 回调函数类型定义（accept_cb/connect_cb 返回失败则不加进事件循环）
typedef int32_t(*accept_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud);          // 接受新连接回调
typedef int32_t(*connect_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t err, ud_cxt *ud); // 连接完成回调
typedef int32_t(*ssl_exchanged_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud); // SSL握手完成回调
typedef void(*recv_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud); // 接收数据回调
typedef void(*send_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, size_t size, ud_cxt *ud); // 发送完成回调
typedef void(*close_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud); // 连接关闭回调
typedef void(*recvfrom_cb)(ev_ctx *ev, SOCKET fd, uint64_t skid, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud); // UDP接收回调
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

#define GET_POS(fd, n) (fd % n)                             // 根据fd计算索引位置
#define GET_PTR(p, n, fd) (1 == n ? p : &p[GET_POS(fd, n)]) // 根据fd获取对应的指针
QUEUE_DECL(off_buf_ctx, qu_off_buf);

// 以下为模块内部公共函数
// 清空发送缓冲队列并释放数据
void _bufs_clear(qu_off_buf_ctx *bufs);
// 设置socket选项：无延迟 + 非阻塞
int32_t _set_sockops(SOCKET fd);
// 创建socket（SOCK_DGRAM/SOCK_STREAM，AF_INET/AF_INET6）
SOCKET _create_sock(int32_t type, int32_t family);
// 创建并绑定监听socket
SOCKET _listen(netaddr_ctx *addr);
// 创建并绑定UDP socket
SOCKET _udp(netaddr_ctx *addr);
// 从socket读取数据（支持SSL/普通）
int32_t _sock_read(SOCKET fd, IOV_TYPE *iov, uint32_t niov, void *arg, size_t *readed);
// 向socket发送数据（支持SSL/普通）
int32_t _sock_send(SOCKET fd, qu_off_buf_ctx *buf_s, size_t *nsend, void *arg);
// 向事件循环发送ud设置命令
void _ev_set_ud(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t type, uint64_t val);
// 直接设置ud_cxt字段值
void _set_ud(ud_cxt *ud, int32_t type, uint64_t val);
// 设置ud_cxt的子协议extra字段
void _set_secextra(ud_cxt *ud, void *val);

#endif//EVPUB_H_
