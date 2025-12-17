#ifndef MONGO_STRUCT_H_
#define MONGO_STRUCT_H_

#include "protocol/mongo/mongo_macro.h"

typedef struct mongo_head {
    int32_t size; // total message size, including this
    int32_t reqid;//id for this message
    int32_t respto;//requestID from the original request(used in responses from the database)
    int32_t prot;//message
}mongo_head;
typedef struct mongo_msg {
    mongo_head head;
    uint32_t flags;//message flags
    char data[0];
}mongo_msg;

typedef struct mongo_ctx {
    uint16_t port;
    int32_t id;
    SOCKET fd;
    uint64_t skid;
    struct task_ctx *task;
    struct evssl_ctx *evssl;
    char authmod[32];
    char ip[IP_LENS];
    char curdb[64];
    char authdb[64];
    char user[64];
    char password[64];
}mongo_ctx;

#endif//MONGO_STRUCT_H_
