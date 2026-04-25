#ifndef CONFIG_H_
#define CONFIG_H_

#include "base/os.h"
//是否启用内存检测
#define MEMORY_CHECK        1
//是否启用SSL
#define WITH_SSL            1
//是否启用LUA
#define WITH_LUA            1

#define KEEPALIVE_TIME      30      // TCP keepalive 空闲时间（秒）
#define KEEPALIVE_INTERVAL  2       // TCP keepalive 探测间隔（秒）
#define CMD_MAX_NREAD       1024    // 命令管道单次最大读取字节数
#define EVENT_WAIT_TIMEOUT  100     // 事件循环等待超时（毫秒）
#define EVENT_CHANGES_CNT   128     // 事件变更队列初始容量
#define INIT_EVENTS_CNT     256     // 初始事件槽位数量
#define DEF_RECV_SIZE       512     // 默认接收缓冲区大小（字节）
#define MAX_RECV_SIZE       4096    // 最大接收缓冲区大小（字节）
#define MAX_RECVFROM_SIZE   4096    // UDP 单次 recvfrom 最大字节数
#define MAX_SEND_SIZE       4096    // 单次发送最大字节数
#define MAX_SEND_NIOV       16      // scatter/gather 发送最大 iov 数量
#define MAX_PACK_SIZE       65536   // 最大数据包大小，0 表示不限制
#define INIT_SENDBUF_LEN    32      // 发送缓冲区初始长度
#define SHRINK_TIME         30000   // 缓冲区收缩检测周期（毫秒）
/* 每隔多少次事件循环才检查一次收缩定时器，避免每次紧循环都调用 clock_gettime */
#define SHRINK_IDLE_CNT     256
#define TIMER_ACCURACY      1000000 // 定时器精度（纳秒，1ms）
// 各模块自旋锁的自旋次数
#define SPIN_CNT_TIMEWHEEL  32      // 时间轮自旋锁次数
#define SPIN_CNT_LOADER     32      // loader 自旋锁次数
#define SPIN_CNT_TASKMSG    32      // task 消息队列自旋锁次数
#define SPIN_CNT_CMD        32      // 命令管道自旋锁次数
#define SPIN_CNT_LSN        32      // 监听器自旋锁次数

#ifdef EV_EPOLL
    #define TRIGGER_ET          1   // epoll 使用边缘触发模式
#endif

#define PACK_TOO_LONG(size) (0 != MAX_PACK_SIZE && (uint64_t)size >= MAX_PACK_SIZE) // 判断数据包是否超过最大限制

#endif//CONFIG_H_
