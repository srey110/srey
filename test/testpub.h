#ifndef TESTPUB_H_
#define TESTPUB_H_

#include "lib.h"
#include "service/synsl.h"

enum {
    SSL_SERVER = 0,
    SSL_CLINET,
};
enum {
    TEST_TIMEOUT = 1000,
    TEST_TCP,
    TEST_SSL,
    TEST_UDP,
    TEST_HTTP,
    TEST_WBSK,
    TEST_SYN
};

#define RAND_CLOSE 1
#define DELAY_SEND 0

#endif
