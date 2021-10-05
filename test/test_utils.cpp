#include "test_utils.h"
#include "utils.h"
#include "timer.h"
#include "thread.h"
#include "netaddr.h"

using namespace srey;

void ctest_utils::test_ntohl64(void)
{
    const uint64_t ulval = 1555655115445474;
    uint64_t ulnet = ntohl64(ulval);
    CPPUNIT_ASSERT_EQUAL(ulnet, 16326828682021045504);

    uint64_t ulhost = ntohl64(ulnet);
    CPPUNIT_ASSERT_EQUAL(ulnet, 16326828682021045504);
}
void ctest_utils::test_threadidprocsnum(void)
{
    uint32_t uiid = threadid();
    uint32_t ucore = procsnum();
    PRINTF("thread id %d, procs number %d\n", uiid, ucore);
    CPPUNIT_ASSERT(true);
}
void ctest_utils::test_format(void)
{
    std::string strcp = "this is format test, 10 5.23";
    std::string str = formatstr("%s %d %.2f", "this is format test,", 10, 5.23);
    CPPUNIT_ASSERT(strcp == str);
}
void ctest_utils::test_file(void)
{
    std::string strpath = getpath();
    PRINTF("program path %s\n", strpath.c_str());

    bool bok = fileexist((strpath + "test.exe").c_str());
    CPPUNIT_ASSERT(bok);
    bok = fileexist(strpath.c_str());
    CPPUNIT_ASSERT(bok);

    bok = isfile((strpath + "test.exe").c_str());
    CPPUNIT_ASSERT(bok);
    bok = isfile(strpath.c_str());
    CPPUNIT_ASSERT(!bok);

    bok = isdir(strpath .c_str());
    CPPUNIT_ASSERT(bok);
    bok = isdir((strpath + "test.exe").c_str());
    CPPUNIT_ASSERT(!bok);

    int64_t isize = filesize((strpath + "Microsoft.DTfW.DHL.manifest").c_str());
    CPPUNIT_ASSERT(isize == 331 || isize == 329);
}
void ctest_utils::test_time(void)
{
    uint64_t uis = nowsec();
    uint64_t uims = nowmsec();
    char as[TIME_LENS] = { 0 };
    char ams[TIME_LENS] = { 0 };

    nowtime("%d/%m/%Y/ %H:%M:%S ", as);
    nowmtime("%d/%m/%Y/ %H:%M:%S ", ams);
    PRINTF("\nsec  %I64d\nmsec %I64d\nsec  %s\nmsec %s", uis, uims, as, ams);

    ctimer timer;
    timer.start();
    USLEEP(1000);
    PRINTF("sleep 1000 us use: %I64d us", timer.elapsed());

    CPPUNIT_ASSERT(true);
}
void ctest_utils::test_hash(void)
{
    const char *pmsg = "this is hash check string.";
    size_t isize = strlen(pmsg);
    uint64_t ulcrc16 = 0x0867;
    uint64_t ulcrc32 = 0x803AC53B;
    uint64_t ulsp = 0xF75E60AAE3C3B7A;
    uint64_t ulmm = 14170620243415901406;

    CPPUNIT_ASSERT(ulcrc16 == crc16(pmsg, isize));
    CPPUNIT_ASSERT(ulcrc32 == crc32(pmsg, isize));
    CPPUNIT_ASSERT(ulsp == siphash64((const uint8_t *)pmsg, isize, 89, 78));
    CPPUNIT_ASSERT(ulmm == murmurhash3(pmsg, isize, 89));
}
void ctest_utils::test_str(void)
{
    std::string str = "가adc가";
    std::string str2 = triml(str);
    CPPUNIT_ASSERT(str2 == str);
    str2 = trimr(str);
    CPPUNIT_ASSERT(str2 == str);

    str = " \r\n\t\v가\nadc가";
    str2 = triml(str);
    CPPUNIT_ASSERT(str2 == "가\nadc가");

    str = "가\nadc가 \r\n\t\v";
    str2 = trimr(str);
    CPPUNIT_ASSERT(str2 == "가\nadc가");

    str = "    \r\n\t\v";
    str2 = trimr(str);
    CPPUNIT_ASSERT(str2 == "");

    str = "    \r\n\t\v";
    str2 = triml(str);
    CPPUNIT_ASSERT(str2 == "");

    str = " \t 가\nadc가 \r\n\t\v";
    str2 = trim(str);
    CPPUNIT_ASSERT(str2 == "가\nadc가");

    str = " \t 가\nadc가 \r\n\t\v\t\t";
    std::vector<std::string> vrtn = split(str, "\t");
    CPPUNIT_ASSERT(vrtn.size() == 5);
}
void ctest_utils::test_sock(void)
{
     SOCKET socks[2];
     bool bok = sockpair(socks);
     CPPUNIT_ASSERT(bok);
     if (!bok)
     {
         return;
     }

     std::string strrecv = "";
     std::string strsend = "this is test." ;
     send(socks[0], strsend.c_str(), (int32_t)strsend.length(), 0);
     int32_t ilens = socknread(socks[1]);
     if (ilens > 0)
     {
         char *pbuf = new(std::nothrow) char[ilens + 1];
         ZERO(pbuf, ilens + 1);
         recv(socks[1], pbuf, ilens, 0);
         strrecv = pbuf;
         SAFE_DELARR(pbuf);
     }

     SAFE_CLOSESOCK(socks[0]);
     SAFE_CLOSESOCK(socks[1]);

     CPPUNIT_ASSERT(strrecv == strsend);
}
