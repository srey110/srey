#include <fstream>
#include "utils.h"
#include "test_utils.h"
#include "test_chan.h"

#ifdef WIN32
#include "../vld/vld.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "vld.lib")
#pragma comment(lib, "lib.lib")
#if _DEBUG
#pragma comment(lib, "cppunitd.lib")
#else
#pragma comment(lib, "cppunit.lib")
#endif//_DEBUG
#endif //OS_WIN

CPPUNIT_TEST_SUITE_REGISTRATION(ctest_chan);
CPPUNIT_TEST_SUITE_REGISTRATION(ctest_utils);

int main(int argc, char* argv[])
{
    std::ofstream xmlrst(srey::getpath() + "test.xml");
    CPPUNIT_NS::Test *suite = CPPUNIT_NS::TestFactoryRegistry::getRegistry().makeTest();
    CPPUNIT_NS::TextUi::TestRunner runner;

    runner.addTest(suite);
    runner.setOutputter(new CPPUNIT_NS::XmlOutputter(&runner.result(), xmlrst));
    runner.run();

    return 0;
}
