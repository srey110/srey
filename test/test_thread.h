#ifndef TEST_LOCK_H_
#define TEST_LOCK_H_

#include "test.h"

void test_fmterror(void);
void test_atomic(void);
void test_mulock(void);
void test_mutrylock(void);
void test_splock(void);
void test_sptrylock(void);
void test_rwlock(void);
void test_tryrwlock(void);

void test_thread(void);

#endif//TEST_LOCK_H_
