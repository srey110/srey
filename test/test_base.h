#ifndef TEST_BASE_H_
#define TEST_BASE_H_

#include "CuTest.h"

// 注册 base 测试套件：内存宏、原子操作、字节序 pack/unpack、bytes / strconv、文件 / 时间 / strptime / 随机 / 进程、timer、once
void test_base(CuSuite *suite);
// 注册会用光 memory.c 计数槽位的用例, 必须排在集成阶段之后跑
void test_base_slots(CuSuite *suite);
// 等进程的分配 / 释放计数静止：日志线程会异步释放之前打出的日志条目，落在两次 mem_stat 之间就会多算释放数。
// 精确比对 mem_stat 的用例取基线前先调它
void test_mem_settle(void);

#endif//TEST_BASE_H_
