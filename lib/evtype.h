#ifndef EVTYPE_H_
#define EVTYPE_H_

#include "macro.h"

#define EV_TIME     0x01
#define EV_ACCEPT   0x02
#define EV_CONNECT  0x03
#define EV_CLOSE    0x04
#define EV_READ     0x05
#define EV_WRITE    0x06
#define EV_ADDLIS   0x07

typedef struct ev_ctx
{
    int32_t evtype;     //类型  
    int32_t code;       //ERR_OK 成功
    void *data;         //用户数据
}ev_ctx;

#endif//EVTYPE_H_
