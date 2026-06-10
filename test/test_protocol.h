
#ifndef TEST_PROTOCOL_H_
#define TEST_PROTOCOL_H_

#include "CuTest.h"

/// <summary>
/// 注册协议层单元测试套件：HTTP、Redis RESP、URL 解析、Custz 打包
/// </summary>
void test_protocol(CuSuite *suite);

#endif//TEST_PROTOCOL_H_
