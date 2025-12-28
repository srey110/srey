#ifndef MONGO_STRUCT_H_
#define MONGO_STRUCT_H_

#include "protocol/mongo/mongo_macro.h"

typedef struct mgopack_ctx {
    int8_t kind;//0 正文 1 文档序列
    uint32_t total; // total message size, including this
    int32_t reqid;//id for this message
    int32_t respto;//requestID from the original request(used in responses from the database)
    int32_t prot;//message
    uint32_t klens;//kind == 1 时有
    uint32_t flags;//message flags
    uint32_t dlens;//doc长度
    char *docid;//文档序列标识符 kind == 1 时有
    char *doc;
    char *payload;
}mgopack_ctx;

typedef struct mongo_ctx {
    uint16_t port;
    int32_t reqid;
    uint32_t flags;//message flags
    SOCKET fd;
    uint64_t skid;
    struct task_ctx *task;
    struct evssl_ctx *evssl;
    struct scram_ctx *scram;
    char *error;
    char ip[IP_LENS];
    char db[64];
    char authdb[64];
    char collection[64];
    char user[64];
    char password[64];
}mongo_ctx;

#endif//MONGO_STRUCT_H_
