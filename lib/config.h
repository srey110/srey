#ifndef CONFIG_H_
#define CONFIG_H_

#include "os.h"

#define MEMORY_CHECK         1
#define PRINT_DEBUG          0
#define WITH_SSL             1

#define CMD_MAX_NREAD       64
#define EVENT_WAIT_TIMEOUT  100
#define EVENT_CHANGES_CNT   128
#define INIT_EVENTS_CNT     256
#define MAX_EVENTS_CNT      2048
#define MAX_RECV_SIZE       4096
#define MAX_RECVFROM_SIZE   4096
#define MAX_SEND_SIZE       4096
#define MAX_SEND_NIOV       16
#define MAX_PACK_SIZE       65535
#define INIT_SENDBUF_LEN    32
#define SHRINK_TIME         (30 * 1000)

#ifdef EV_EPOLL
#define TRIGGER_ET          1
#endif

#endif//CONFIG_H_
