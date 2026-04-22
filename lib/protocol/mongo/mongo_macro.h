#ifndef MONGO_MACRO_H_
#define MONGO_MACRO_H_

#include "base/macro.h"

typedef enum mongo_flags {
    CHECKSUM = 0x01,
    MORETOCOME = 0x02,
    EXHAUSTALLOWED = 1 << 16,
}mongo_flags;
typedef enum mongo_prot {
    OP_COMPRESSED = 2012,
    OP_MSG = 2013
}mongo_prot;

#endif//MONGO_MACRO_H_
