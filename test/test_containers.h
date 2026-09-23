#ifndef TEST_CONTAINERS_H_
#define TEST_CONTAINERS_H_

#include "CuTest.h"

// 注册容器测试套件：mpq、bbq、spsc、fsqu、chan、hashmap、heap、queue、sarray、slist、rbtree
void test_containers(CuSuite *suite);
// 游标不带版本号那一支的 hashmap iter / scan 用例，在 test_hashmap_unver.c 里单独编（见该文件头），由容器套件注册
void test_hashmap_iter_unversioned(CuTest *tc);

#endif//TEST_CONTAINERS_H_
