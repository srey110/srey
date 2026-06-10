#ifndef TEST_SERI_H_
#define TEST_SERI_H_

#include "CuTest.h"

// 注册 seri 序列化测试套件：基本类型往返、int 各档边界、字符串短/长、嵌套 table、错误流处理
void test_seri(CuSuite *suite);

#endif//TEST_SERI_H_
