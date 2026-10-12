#ifndef TEST_UTILS_H_
#define TEST_UTILS_H_

#include "CuTest.h"

// 注册 utils 测试套件：binary、buffer、sfid、uuid、hash_ring、netaddr、log、popen、tw
void test_utils(CuSuite *suite);

#endif//TEST_UTILS_H_
