#ifndef TEST_THREAD_H_
#define TEST_THREAD_H_

#include "CuTest.h"

// 注册线程原语测试套件：mutex、spinlock、rwlock、cond、thread
void test_thread(CuSuite *suite);

#endif//TEST_THREAD_H_
