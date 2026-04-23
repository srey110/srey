#ifndef TEST_CRYPT_H_
#define TEST_CRYPT_H_

#include "CuTest.h"

/// <summary>
/// 注册加密测试套件：base64、crc、digest、hmac、urlraw、xor
/// </summary>
void test_crypt(CuSuite *suite);

#endif//TEST_CRYPT_H_
