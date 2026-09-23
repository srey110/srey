#ifndef CONFIG_H_
#define CONFIG_H_

#include "base/os.h"

//是否启用内存检测，开发环境监测是否有内存泄漏，可与MEMORY_TRACE配合使用。
//计数走分条计数器(见 memory.c)，多线程下与关掉几乎同价，正常无需关
#ifndef MEMORY_CHECK
    #define MEMORY_CHECK 1
#endif
//是否追踪分配调用栈,退出时 dump 未释放块的 backtrace(需 MEMORY_CHECK 同时为 1,有性能开销)
#ifndef MEMORY_TRACE
    #define MEMORY_TRACE 0
#endif
//是否启用mimalloc
#ifndef WITH_MIMALLOC
    #define WITH_MIMALLOC 0
#endif
//是否启用SSL
#ifndef WITH_SSL
    #define WITH_SSL 1
#endif
//是否启用LUA
#ifndef WITH_LUA
    #define WITH_LUA 1
#endif
//是否使用lua bytecache
#ifndef ENABLE_LUA_BYTECACHE
    #define ENABLE_LUA_BYTECACHE 1
#endif
//是否启用消息分发 CPU 耗时统计
#ifndef ENABLE_DISPATCH_STAT
    #define ENABLE_DISPATCH_STAT 0
#endif

#define KEEPALIVE_TIME      30 // TCP keepalive 空闲时间（秒）
#define KEEPALIVE_INTERVAL  2 // TCP keepalive 探测间隔（秒）
// 单次读向 buffer 要多大空间（字节），不是缓冲上限。读满说明 socket 里还有,
// 下一轮按它翻倍再要一次、到 2 倍封顶（见 buffer_from_sock），大块接收的读次数因此减半
#define MAX_RECV_SIZE       4096
// 单 sock 接收缓冲堆积上限（字节）：协议层消费完仍超过它，说明对端在灌凑不成包的数据，
// 打日志并断连（close_type 为 LOCAL）。0 表示不限制。与上面的 MAX_RECV_SIZE 无关——
// 那个管单次读多少，这个管总共攒多少。设值须不小于所启用协议的最大包长，否则合法大包会被
// 误断：见 prots_pub.h，REDIS_MAX_BULK_LENS 512MB、MONGO_MAX_PACK_LENS 64MB 是其中最大的两个
#define MAX_RECV_CASH       0 // 0 表示不限制
#define MAX_RECVFROM_SIZE   (64 * ONEK)// UDP 单次 recvfrom 最大字节数
// 单次发送(一次 writev/WSASend)的字节上限;0 表示不限制,只受 MAX_SEND_NIOV 的条数约束。
// 判定在填完一条 iov 之后做,所以单条 buf 超过它时那次发送只带这一条 ——
// 取小值会让排空积压要多打好几次系统调用,而拷贝总量并不因此减少
#define MAX_SEND_SIZE       0 // 0 表示不限制
#define MAX_SSL_SEND_SIZE   4096 // 单次 SSL_write 最大字节数；须小到 socket 一次吃得下整条 TLS 记录，调大的后果见 _evpub_sock_send_ssl
#define MAX_SEND_NIOV       16 // scatter/gather 发送最大 iov 数量
#define MAX_EXPAND_NIOV     4 // scatter/gather 接收最大 iov 数量
#define MAX_SENDQ_CNT       ONEK // 单 sock 发送队列上限(buf 数)；超限 TCP 丢数据并断连、UDP 丢包；0 表示不限制
#define SSL_WPEND_MAX_MS    30000 // 挂起的 SSL 写零进展上限（毫秒），超限判死断连；须 > 0，取 0 不是关闭而是所有挂起写立刻判死

//内核 reuseport 对 TCP 监听做不做连接级分发。为 1 时 accept 出的连接直接留在 accept 它的
//event 线程,省掉一次跨线程投递;为 0 时按 fd 取模重新分配。
//Linux 3.9+ 的 SO_REUSEPORT 与 FreeBSD 12+ 的 SO_REUSEPORT_LB 会把新连接按四元组散到各 listen fd;
//macOS 与其余 BSD 的裸 SO_REUSEPORT 只允许重复绑定,连接全落最后 bind 的那个,就地挂会退化成单线程。
//Solaris/AIX 有 SO_REUSEPORT 但分发语义未验证,按不分发处理
#ifndef REUSEPORT_BALANCED
    //必须带上 SO_REUSEPORT：ev_listen 正是按它决定建 nthreads 个 listen fd 还是只建 1 个,
    //少了它 nlsn 会是 1、所有连接都从同一个线程 accept,就地挂立刻退化成单线程
    #if defined(SO_REUSEPORT) && (defined(SO_REUSEPORT_LB) || defined(OS_LINUX))
        #define REUSEPORT_BALANCED 1
    #else
        #define REUSEPORT_BALANCED 0
    #endif
#endif
// fsqu 的快路径用哪个,根据 test 里的 benchmark(bench_mpq)决定。
// 非 0 都是"无锁环 + 满了降级到无界溢出层",0 是整条队列一把自旋锁
#if defined(OS_DARWIN) || defined(OS_BSD)
    #define FSQU_FAST_MODEL 0 //queue+spin
#else
    #define FSQU_FAST_MODEL 1 //mpq 无锁有界环;2 = bbq 无锁分块环(容量上限 2^20,超了 init 中止,见 bbq.h)
#endif

#endif//CONFIG_H_
