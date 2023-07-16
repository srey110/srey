#ifndef TESTPUB_H_
#define TESTPUB_H_

#include "lib.h"
#include "service/synsl.h"

enum {
    SSL_SERVER = 0,
    SSL_CLINET,
};
enum {
    TEST1 = 1000,
    TEST2,
    TEST3,
    TEST4,
    TEST5,
    TEST6
};

#define RAND_CLOSE 1
#define DELAY_SEND 0

#endif
