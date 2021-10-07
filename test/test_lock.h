#ifndef TEST_LOCK_H_
#define TEST_LOCK_H_

#include "test.h"

class ctest_lock : public CPPUNIT_NS::TestFixture
{
    CPPUNIT_TEST_SUITE(ctest_lock);

    CPPUNIT_TEST(test_atomic);
    CPPUNIT_TEST(test_mulock);
    CPPUNIT_TEST(test_mutrylock);
    CPPUNIT_TEST(test_splock);
    CPPUNIT_TEST(test_sptrylock);
    CPPUNIT_TEST(test_rwlock);
    CPPUNIT_TEST(test_tryrwlock);

    CPPUNIT_TEST_SUITE_END();

public:
    ctest_lock(void) {};
    ~ctest_lock(void) {};

private:
    void test_atomic(void);
    void test_mulock(void);
    void test_mutrylock(void);
    void test_splock(void);
    void test_sptrylock(void);  
    void test_rwlock(void);
    void test_tryrwlock(void);

private:
    void _testlock(void(*lockfunc)(void *pparam));
    void _testrwlock(void(*wfunc)(void *pparam), void(*rfunc)(void *pparam));
};

#endif//TEST_LOCK_H_
