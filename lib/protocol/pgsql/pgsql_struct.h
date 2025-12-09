#ifndef PGSQL_STRUCT_H_
#define PGSQL_STRUCT_H_

#include "srey/spub.h"
#include "protocol/pgsql/pgsql_scram.h"

#define PGSQL_SALT_LENS 16

typedef struct pgpack_ctx {
    int8_t prot;
    char *payload;
    void *pack;
    void(*_free_pgpack)(void *);
}pgpack_ctx;
typedef struct pgpack_err {//错误
    int8_t code;
}pgpack_err;

typedef struct pgsql_ctx {
    int8_t scrammod;
    int8_t readyforquery;//'I': 空闲（不在事务块中） 'T': 事务块中  'E':失败的事务块中
    uint16_t port;
    int32_t status;
    int32_t pid;//后端的进程ID
    uint32_t key;//后端的秘钥
    SOCKET fd;
    uint64_t skid;
    struct task_ctx *task;
    struct evssl_ctx *evssl;
    scram_ctx *scram;
    pgpack_ctx *pack;
    char ip[IP_LENS];
    char user[64];
    char password[64];
    char database[64];
    char error_msg[256];
}pgsql_ctx;

#endif//PGSQL_STRUCT_H_
