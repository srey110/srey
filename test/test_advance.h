#ifndef TEST_ADVANCE_H_
#define TEST_ADVANCE_H_

#include "CuTest.h"

// 注册 advance 层测试套件：router 路径规范化
void test_advance(CuSuite *suite);
// router_tree 段级路由树用例，由 test_advance 统一注册
void test_router_tree(CuSuite *suite);

#endif//TEST_ADVANCE_H_
