#ifndef EVTYPE_H_
#define EVTYPE_H_

#include "macro.h"

SREY_NS_BEGIN

enum EVTYPE
{
    EV_TIME = 0,
    EV_ACCEPT,
    EV_CONNECT,
    EV_CLOSE,
    EV_READ,
    EV_WRITE,
    EV_ADDLIS
};

struct event
{
    EVTYPE evtype;      //类型  
    int32_t code;       //ERR_OK 成功
    void *data;         //用户数据
};

SREY_NS_END

#endif//EVTYPE_H_
