#ifndef TEST_CORO_H_
#define TEST_CORO_H_

#include "CuTest.h"

// 注册通用协程调度器(lib/coro)测试套件：不起 loader，直接驱动 coro_ctx
void test_coro(CuSuite *suite);

#endif//TEST_CORO_H_
