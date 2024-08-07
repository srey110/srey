#ifndef CONFIG_H_
#define CONFIG_H_

#include "base/os.h"
//是否开启内存检查
#define MEMORY_CHECK        0
//是否使用SSL
#define WITH_SSL            0
//是否使用LUA
#define WITH_LUA            1
//是否使用协程
#define WITH_CORO           1

#define KEEPALIVE_TIME      30
#define KEEPALIVE_INTERVAL  2
#define CMD_MAX_NREAD       1024
#define EVENT_WAIT_TIMEOUT  100
#define EVENT_CHANGES_CNT   128
#define INIT_EVENTS_CNT     256
#define DEF_RECV_SIZE       512
#define MAX_RECV_SIZE       4096
#define MAX_RECVFROM_SIZE   4096
#define MAX_SEND_SIZE       4096
#define MAX_SEND_NIOV       16
#define MAX_PACK_SIZE       65536
#define INIT_SENDBUF_LEN    32
#define SHRINK_TIME         30000
#define TIMER_ACCURACY      1000000
//spin spcnt值
#define SPIN_CNT_TIMEWHEEL  32
#define SPIN_CNT_LOADER     32
#define SPIN_CNT_TASKMSG    32
#define SPIN_CNT_CMD        32
#define SPIN_CNT_LSN        32

#ifdef EV_EPOLL
#define TRIGGER_ET          1
#endif

#define PACK_TOO_LONG(size) (0 != MAX_PACK_SIZE && (uint64_t)size >= MAX_PACK_SIZE)

#endif//CONFIG_H_
