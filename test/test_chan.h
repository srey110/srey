#ifndef TEST_CHAN_H_
#define TEST_CHAN_H_

#include "test.h"

using namespace SREY_NS;

class ctest_chan : public CPPUNIT_NS::TestFixture
{
    CPPUNIT_TEST_SUITE(ctest_chan);

    CPPUNIT_TEST(test_buffchan);
    CPPUNIT_TEST(test_unbuffchan);
    CPPUNIT_TEST(test_bufselect);

    CPPUNIT_TEST_SUITE_END();

public:
    ctest_chan(void) {};
    ~ctest_chan(void) {};

private:
    void test_buffchan(void);
    void test_unbuffchan(void);
    void test_bufselect(void);

private:
    void _test_chan(int32_t icap, bool bselect);

private:
    cthread *pth1;
    cthread *pth2;
    cchan **pchans;    
};

#endif//TEST_CHAN_H_
