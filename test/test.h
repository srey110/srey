#ifndef TEST_H_
#define TEST_H_

#include "lib.h"

#define TEST_ASSERT(exp)\
do\
{\
    if (!(exp))\
    {\
        PRINTF("%s", "================================test faild.================================");\
    }\
} while (0);

#endif//TEST_H_
