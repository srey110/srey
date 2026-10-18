#ifndef CONFIG_H_
#define CONFIG_H_

#include "base/os.h"

//是否启用内存检测，开发环境监测是否有内存泄漏，可与MEMORY_TRACE配合使用。
//计数走分条计数器(见 memory.c)，每次分配/释放多一次原子加；aarch64 Linux 上 gcc 把它编成外联调用，开销比别的平台明显
#ifndef MEMORY_CHECK
    #define MEMORY_CHECK 1
#endif
//是否追踪分配调用栈,退出时 dump 未释放块的 backtrace(需 MEMORY_CHECK 同时为 1,有性能开销)
#ifndef MEMORY_TRACE
    #define MEMORY_TRACE 0
#endif
// 下面 WITH_MIMALLOC / WITH_SSL / WITH_LUA 三个还被 mk.sh / CMakeLists.txt / tools/deps.py 按文本读，
// 只认 `#define WITH_X 0|1` 这一行、不认 #if，据此决定链哪些库、编哪些依赖。
// 要按平台关掉某个，得在这三个脚本里同步处理；只在这里加平台条件，脚本与编译器取到的值会不一致
//是否启用mimalloc
#ifndef WITH_MIMALLOC
    #define WITH_MIMALLOC 1
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
// 下一轮按它翻倍再要、到 RECV_GROW_CAP 封顶（见 buffer.h），大块接收的读次数因此少很多。
// kqueue 平台知道可读字节数，首轮直接按它要（封顶 RECV_HINT_CAP）；协议层知道本条消息还差多少（buffer_want），也按它要（封顶 RECV_GROW_CAP）
#define MAX_RECV_SIZE       4096
// 单 sock 接收缓冲堆积上限（字节）：协议层消费完仍超过它，说明对端在灌凑不成包的数据，
// 打日志并断连（close_type 为 LOCAL）。0 表示不限制。与上面的 MAX_RECV_SIZE 无关——
// 那个管单次读多少，这个管总共攒多少。设值须不小于所启用协议的最大包长，否则合法大包会被
// 误断：见 prots_pub.h，REDIS_MAX_BULK_LENS 512MB、MONGO_MAX_PACK_LENS 64MB 是其中最大的两个
#define MAX_RECV_CASH       0 // 0 表示不限制
#define MAX_RECVFROM_SIZE   (64 * 1024)// UDP 单次 recvfrom 最大字节数
// 单次发送(一次 writev/WSASend)的字节上限;0 表示不限制,只受 MAX_SEND_NIOV 的条数约束。
// 判定在填完一条 iov 之后做,所以单条 buf 超过它时那次发送只带这一条 ——
// 取小值会让排空积压要多打好几次系统调用,而拷贝总量并不因此减少
#define MAX_SEND_SIZE       0 // 0 表示不限制
#define MAX_SSL_SEND_SIZE   4096 // 单次 SSL_write 最大字节数；须小到 socket 一次吃得下整条 TLS 记录，调大的后果见 _evpub_sock_send_ssl
// scatter/gather 发送最大 iov 数量；单连接一轮回包攒满这么多条就先发一批，是少发几次与让对端早拿到回包的折中。
// 不能超过系统的 IOV_MAX(AIX / Solaris / HP-UX 只有 16)，超了 writev 直接报错断连
#if defined(IOV_MAX) && IOV_MAX < 32
    #define MAX_SEND_NIOV   IOV_MAX
#else
    #define MAX_SEND_NIOV   32
#endif
#define MAX_EXPAND_NIOV     4 // scatter/gather 接收最大 iov 数量
#define MAX_SENDQ_CNT       1024 // 单 sock 发送队列上限(buf 数)；超限 TCP 丢数据并断连、UDP 丢包；0 表示不限制
#define SSL_WPEND_MAX_MS    30000 // 挂起的 SSL 写零进展上限（毫秒），超限判死断连；须 > 0，取 0 不是关闭而是所有挂起写立刻判死

//内核 reuseport 对 TCP 监听做不做连接级分发。为 1 时 accept 出的连接直接留在 accept 它的
//event 线程,省掉一次跨线程投递;为 0 时按 fd 取模重新分配。
//Linux 3.9+ 的 SO_REUSEPORT 与 FreeBSD 12+ 的 SO_REUSEPORT_LB 会把新连接按四元组散到各 listen fd;
//macOS 与其余 BSD 的裸 SO_REUSEPORT 只允许重复绑定,连接全落最后 bind 的那个,就地挂会退化成单线程。
//Solaris/AIX 有 SO_REUSEPORT 但分发语义未验证,按不分发处理
#ifndef REUSEPORT_BALANCED
    //必须带上 SO_REUSEPORT：ev_listen 正是按它决定每个收连接的线程各建一个 listen fd(没给线程表就是 nthreads 个)还是只建 1 个,
    //少了它 nlsn 会是 1、所有连接都从同一个线程 accept,就地挂立刻退化成单线程
    #if defined(SO_REUSEPORT) && (defined(SO_REUSEPORT_LB) || defined(OS_LINUX))
        #define REUSEPORT_BALANCED 1
    #else
        #define REUSEPORT_BALANCED 0
    #endif
#endif
// waiter 的睡眠用哪种实现(见 thread/futex.h)：1 按地址等待，0 mutex + cond，要回退就改成 0。
// 默认 Linux / FreeBSD / Windows(8 起)为 1；macOS 要 14.4 起的系统与 SDK，SDK 里没有 os_sync 时默认 0；其余平台只能 0
#ifndef WITH_FUTEX
    #if defined(OS_LINUX) || defined(OS_FBSD) || defined(OS_WIN) || (defined(OS_DARWIN) && defined(HAVE_OS_SYNC))
        #define WITH_FUTEX 1
    #else
        #define WITH_FUTEX 0
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
