#ifndef CONFIG_H_
#define CONFIG_H_

#include "base/os.h"

//是否启用内存检测
#define MEMORY_CHECK        1
//是否追踪分配调用栈,退出时 dump 未释放块的 backtrace(需 MEMORY_CHECK 同时为 1,有性能开销)
#define MEMORY_TRACE        0
//是否启用SSL
#define WITH_SSL            1
//是否启用LUA
#define WITH_LUA            1
//是否使用lua bytecache
#define ENABLE_LUA_BYTECACHE 1
//是否启用消息分发 CPU 耗时统计
#define ENABLE_DISPATCH_STAT 1

#define TIMER_ACCURACY      1000000 // 定时器精度（纳秒，1ms）
#define KEEPALIVE_TIME      30      // TCP keepalive 空闲时间（秒）
#define KEEPALIVE_INTERVAL  2       // TCP keepalive 探测间隔（秒）
#define CMD_MAX_NREAD       128     // 命令单次读取最大数量
#define QUEUE_OVERLOAD_RATIO 3      // 队列积压告警初始阈值 = 容量 / RATIO，触发后翻倍，空队列重置
#define EVENT_WAIT_TIMEOUT  100     // 事件循环等待超时（毫秒）
#define EVENT_CHANGES_CNT   128     // 事件变更队列初始容量
#define INIT_EVENTS_CNT     256     // 初始事件槽位数量
#define MAX_RECV_SIZE       4096    // 最大接收缓冲区大小（字节）
#define MAX_RECVFROM_SIZE   (64 * ONEK)// UDP 单次 recvfrom 最大字节数
#define MAX_SEND_SIZE       4096    // 单次发送最大字节数
#define MAX_SEND_NIOV       16      // scatter/gather 发送最大 iov 数量
#define MAX_PACK_SIZE       65535   // 最大数据包大小，0 表示不限制
#define MAX_SENDQ_CNT       512     // 单 sock 发送队列上限(buf 数)；超限 TCP 丢数据并断连、UDP 丢包；0 表示不限制
#define WB_WARN_INIT_SIZE   (1024 * 1024) // 单 sock 发送缓冲字节告警首阈值；触发后翻倍（1MB→2MB→4MB...），队列清空后复位；0 表示禁用
#define INIT_SENDBUF_LEN    32      // 发送缓冲区初始长度
#define SHRINK_TIME         10000   // 缓冲区收缩检测周期（毫秒）
#define SHRINK_NKEEP(n)  ((n) - (n) / 5) // pool_shrink 的 keep 量
#define SHRINK_BUSY      4, 5 // pool_shrink 的 load_trend busy 判定比例 num/den:空闲骤降至上次的 4/5 以下视为忙,跳过本次收缩
#define QTN_MS              500     // 释放对象隔离时间(毫秒)，应大于一轮 kevent 周期
#define EVENT_CHECK_INTERVAL 10     // 每隔多少次事件循环才检查一次定时器，避免每次紧循环都调用 clock_gettime
#define SPIN_CNT             32     // 自旋次数

#ifdef EV_EPOLL
    #define TRIGGER_ET          1   // epoll 使用边缘触发模式
#endif
//FSQU_MPQ CMD_PIPE_QU 根据test里面的benchmark决定
//判断fsqu队列使用mpq 还是queue+spin
#if defined(OS_DARWIN) || defined(OS_BSD)
    #define FSQU_MPQ 0 //queue+spin
#else
    #define FSQU_MPQ 1 //mpq
#endif
//判断是否直接pipe发命令，还是需要队列作为载体.
#if defined(OS_DARWIN) || defined(OS_BSD)
    #define CMD_PIPE_QU 1 //pipe + qu(queue类型由FSQU_MPQ确定)
#else
    #define CMD_PIPE_QU 0 //pipe
#endif

#define PACK_TOO_LONG(size) (0 != MAX_PACK_SIZE && (uint64_t)(size) >= MAX_PACK_SIZE) // 判断数据包是否超过最大限制

#endif//CONFIG_H_
