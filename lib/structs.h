#ifndef STRUCTS_H_
#define STRUCTS_H_

#include "macro.h"

/*
用户数据
timeout: data(task_ctx) session
net: pktype(pack_type) data(task_ctx) session nretry  http(extra)
*/
typedef struct ud_cxt {
    uint8_t pktype;
    uint8_t status;
    uint8_t nretry;
    uint8_t svside;
    void *data;
    void *extra;
    void *hscb;
    uint64_t session;
}ud_cxt;

#define COPY_UD(dst, src)\
do {\
    if (NULL != (src)){\
        (dst) = *(src);\
    }else{\
        ZERO(&(dst), sizeof(ud_cxt));\
    }\
} while (0)

#endif//STRUCTS_H_
