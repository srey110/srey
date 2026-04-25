#ifndef MONGO_MACRO_H_
#define MONGO_MACRO_H_

#include "base/macro.h"

typedef enum mongo_flags {
    CHECKSUM = 0x01,          //末尾附带 CRC-32C 校验和
    MORETOCOME = 0x02,        //发送方还有后续消息，接收方不必回复当前消息
    EXHAUSTALLOWED = 1 << 16, //客户端支持 moreToCome 的多消息响应
}mongo_flags;
typedef enum mongo_prot {
    OP_COMPRESSED = 2012, //压缩消息
    OP_MSG = 2013         //标准消息（MongoDB Wire Protocol OP_MSG）
}mongo_prot;

#endif//MONGO_MACRO_H_
