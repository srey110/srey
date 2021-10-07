#include <fstream>
#include "test_utils.h"
#include "test_chan.h"
#include "test_lock.h"

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

CPPUNIT_TEST_SUITE_REGISTRATION(ctest_lock);
CPPUNIT_TEST_SUITE_REGISTRATION(ctest_chan);
CPPUNIT_TEST_SUITE_REGISTRATION(ctest_utils);

int main(int argc, char* argv[])
{
#ifdef OS_WIN
    WSAData wsData;
    WORD wVersionReq(MAKEWORD(2, 2));
    uint32_t irtn = WSAStartup(wVersionReq, &wsData);
    if (0 != irtn)
    {
        PRINTF("%s", "WSAStartup version 2.2 error.");
        return 1;
    }
#endif

    std::string xmlpath = srey::getpath() + PATH_SEPARATOR + "test.xml";
    std::ofstream xmlrst(xmlpath.c_str());
    CPPUNIT_NS::Test *suite = CPPUNIT_NS::TestFactoryRegistry::getRegistry().makeTest();
    CPPUNIT_NS::TextUi::TestRunner runner;
    runner.addTest(suite);
    runner.setOutputter(new CPPUNIT_NS::XmlOutputter(&runner.result(), xmlrst));
    runner.run();

#ifdef OS_WIN
    WSACleanup();
#endif

    return 0;
}
