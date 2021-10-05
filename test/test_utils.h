#ifndef TEST_UTILS_H_
#define TEST_UTILS_H_

#include "test.h"

class ctest_utils : public CPPUNIT_NS::TestFixture
{
    CPPUNIT_TEST_SUITE(ctest_utils);

    CPPUNIT_TEST(test_ntohl64);
    CPPUNIT_TEST(test_threadidprocsnum);
    CPPUNIT_TEST(test_format);
    CPPUNIT_TEST(test_file);
    CPPUNIT_TEST(test_time);
    CPPUNIT_TEST(test_hash);
    CPPUNIT_TEST(test_str);
    CPPUNIT_TEST(test_sock);

    CPPUNIT_TEST_SUITE_END();

public:
    ctest_utils(void) {};
    ~ctest_utils(void) {};

private:
    void test_ntohl64(void);
    void test_threadidprocsnum(void);
    void test_format(void);
    void test_file(void);
    void test_time(void);
    void test_hash(void);
    void test_str(void);
    void test_sock(void);
};

#endif//TEST_UTILS_H_
