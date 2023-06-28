#ifndef STRUCTS_H_
#define STRUCTS_H_

#include "macro.h"

/*
用户数据
*/
typedef struct ud_cxt {
    uint8_t pktype;//数据包类型
    uint8_t status;//解包状态
    uint8_t nretry;//解析一个包，重试次数
    uint8_t synflag;//同步读,链接
    void *data;//task_ctx
    void *extra;//数据包
    uint64_t session;
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
