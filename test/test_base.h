#ifndef TEST_BASE_H_
#define TEST_BASE_H_

#include "CuTest.h"

/// <summary>
/// 注册 base 测试套件：内存宏、原子操作
/// </summary>
void test_base(CuSuite *suite);
// 注册会用光 memory.c 计数槽位的用例, 必须排在集成阶段之后跑
void test_base_slots(CuSuite *suite);

#endif//TEST_BASE_H_
