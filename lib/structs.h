#ifndef STRUCTS_H_
#define STRUCTS_H_

#include "macro.h"

/*
用户数据
timeout: data(task_ctx) session status
net: pktype(pack_type) data(task_ctx) session nretry  http(extra)
*/
typedef struct ud_cxt {
    uint8_t pktype;
    uint8_t status;
    uint8_t nretry;
    uint8_t synflag;
    void *data;
    void *extra;
    void *arg;
    uint64_t session;
    uint64_t skid;
}ud_cxt;
typedef struct buf_ctx {
    void *data;
    size_t lens;
}buf_ctx;
typedef struct off_buf_ctx {
    void *data;
    size_t len;
    size_t offset;
}off_buf_ctx;

#define COPY_UD(dst, src)\
do {\
    if (NULL != (src)){\
        (dst) = *(src);\
    }else{\
        ZERO(&(dst), sizeof(ud_cxt));\
    }\
} while (0)

#endif//STRUCTS_H_
