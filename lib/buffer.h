#ifndef BUFFER_H_
#define BUFFER_H_

#include "macro.h"

SREY_NS_BEGIN

class cbuffer
{
public:
    cbuffer();
    ~cbuffer();
    bool add(void *data, size_t datlen);
    void drain() {};

private:
    //存放数据起始位置
    uint8_t *buffer;
    //buffer起始地址
    uint8_t *orig_buffer;
    //buffer起始地址与数据存放地址的偏移
    size_t misalign;
    //总共buffer的长度
    size_t totallen;
    //缓冲区数据长度
    size_t off;
};

SREY_NS_END

#endif//BUFFER_H_
