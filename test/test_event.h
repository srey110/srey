#ifndef TEST_EVENT_H_
#define TEST_EVENT_H_

#include "CuTest.h"

// 注册 event 层测试套件：关闭前冲刷、FIN 检出、close_type 三档
void test_event(CuSuite *suite);

#endif//TEST_EVENT_H_
