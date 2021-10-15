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
    EV_WRITE
};

struct EV
{
    EVTYPE evtype;      //类型
    class cchan *chan;  //接收消息的chan
    void *data;         //用户数据

    virtual ~EV() {};
};

SREY_NS_END

#endif//EVTYPE_H_
