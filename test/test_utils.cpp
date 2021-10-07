#include "test_utils.h"

using namespace srey;

void ctest_utils::test_threadidprocsnum(void)
{
    PRINTF("%s", "test_threadidprocsnum");
    uint32_t uiid = threadid();
    uint32_t ucore = procsnum();
    PRINTF("thread id %d, procs number %d\n", uiid, ucore);
    CPPUNIT_ASSERT(true);
}
void ctest_utils::test_format(void)
{
    PRINTF("%s", "test_format");
    std::string strcp = "this is format test, 10 5.23";
    std::string str = formatstr("%s %d %.2f", "this is format test,", 10, 5.23);
    CPPUNIT_ASSERT(strcp == str);
}
void ctest_utils::test_file(void)
{
    PRINTF("%s", "test_file");
    std::string strpath = getpath();
    PRINTF("program path %s\n", strpath.c_str());

    std::list<std::string> lstallfile;
    filefind(strpath.c_str(), lstallfile);

    lstallfile.clear();
    filefind(strpath.c_str(), lstallfile, true);
    
    const char *pfilename = "Microsoft.DTfW.DHL.manifest";
    bool bok = fileexist((strpath + PATH_SEPARATOR + pfilename).c_str());
    CPPUNIT_ASSERT(bok);
    bok = fileexist(strpath.c_str());
    CPPUNIT_ASSERT(bok);

    bok = isfile((strpath + PATH_SEPARATOR + pfilename).c_str());
    CPPUNIT_ASSERT(bok);
    bok = isfile(strpath.c_str());
    CPPUNIT_ASSERT(!bok);

    bok = isdir(strpath .c_str());
    CPPUNIT_ASSERT(bok);
    bok = isdir((strpath + PATH_SEPARATOR + pfilename).c_str());
    CPPUNIT_ASSERT(!bok);

    int64_t isize = filesize((strpath + PATH_SEPARATOR + pfilename).c_str());
    CPPUNIT_ASSERT(isize == 331 || isize == 329);
}
void ctest_utils::test_time(void)
{
    PRINTF("%s", "test_time");
    char as[TIME_LENS] = { 0 };
    char ams[TIME_LENS] = { 0 };

    nowtime("%d/%m/%Y/ %H:%M:%S ", as);
    nowmtime("%d/%m/%Y/ %H:%M:%S ", ams);
    PRINTF("\nsec%s\nmsec %s", as, ams);

    ctimer timer;
    timer.start();
    USLEEP(1000);
    PRINTF("sleep 1000 us use: %d us", (int32_t)timer.elapsed());

    CPPUNIT_ASSERT(true);
}
void ctest_utils::test_hash(void)
{
    PRINTF("%s", "test_hash");
    const char *plmsg = "Independent implementation of MD5 (RFC 1321).\
        This code implements the MD5 Algorithm defined in RFC 1321, whosetext is available at\
        http ://www.ietf.org/rfc/rfc1321.txt\
    The code is derived from the text of the RFC, including the test suite\
    (section A.5) but excluding the rest of Appendix A.It does not include\
        any code or documentation that is identified in the RFC as being\
        copyrighted.\
        The original and principal author of md5.h is L.Peter Deutsch\
        <ghost@aladdin.com>.Other authors are noted in the change history\
        that follows(in reverse chronological order) :\
        2002 - 04 - 13 lpd Removed support for non - ANSI compilers; removed\
        references to Ghostscript; clarified derivation from RFC 1321;\
    now handles byte order either statically or dynamically.\
        1999 - 11 - 04 lpd Edited comments slightly for automatic TOC extraction.\
        1999 - 10 - 18 lpd Fixed typo in header comment(ansi2knr rather than md5);\
    added conditionalization for C++ compilation from Martin\
        Purschke <purschke@bnl.gov>.\
        1999 - 05 - 03 lpd Original version.";
    char md5str[33];
    size_t ilens = strlen(plmsg);
    md5(plmsg, ilens, md5str);
    char *pmd5 = toupper(md5str);
    CPPUNIT_ASSERT(std::string("D0132A3ADAAA35FD3DDFA005A9AE21C1") == std::string(pmd5));

    char sha1str[20];
    sha1(plmsg, ilens, sha1str);
    std::string str = tohex(sha1str, sizeof(sha1str));
    CPPUNIT_ASSERT("3E 3E 5F 53 D8 A2 49 B3 DB DB 90 95 86 76 DE B2 1C 14 07 4F" == str);
    str = tohex(sha1str, sizeof(sha1str), false);
    CPPUNIT_ASSERT("3E3E5F53D8A249B3DBDB90958676DEB21C14074F" == str);

    size_t icodelens = B64_ENSIZE(ilens);
    char *pbuf = new(std::nothrow) char[icodelens];
    int32_t irtn = b64encode(plmsg, ilens, pbuf);
    CPPUNIT_ASSERT(std::string(pbuf, irtn) == "SW5kZXBlbmRlbnQgaW1wbGVtZW50YXRpb24gb2YgTUQ1IChSRkMgMTMyMSkuICAgICAgICBUaGlzIGNvZGUgaW1wbGVtZW50cyB0aGUgTUQ1IEFsZ29yaXRobSBkZWZpbmVkIGluIFJGQyAxMzIxLCB3aG9zZXRleHQgaXMgYXZhaWxhYmxlIGF0ICAgICAgICBodHRwIDovL3d3dy5pZXRmLm9yZy9yZmMvcmZjMTMyMS50eHQgICAgVGhlIGNvZGUgaXMgZGVyaXZlZCBmcm9tIHRoZSB0ZXh0IG9mIHRoZSBSRkMsIGluY2x1ZGluZyB0aGUgdGVzdCBzdWl0ZSAgICAoc2VjdGlvbiBBLjUpIGJ1dCBleGNsdWRpbmcgdGhlIHJlc3Qgb2YgQXBwZW5kaXggQS5JdCBkb2VzIG5vdCBpbmNsdWRlICAgICAgICBhbnkgY29kZSBvciBkb2N1bWVudGF0aW9uIHRoYXQgaXMgaWRlbnRpZmllZCBpbiB0aGUgUkZDIGFzIGJlaW5nICAgICAgICBjb3B5cmlnaHRlZC4gICAgICAgIFRoZSBvcmlnaW5hbCBhbmQgcHJpbmNpcGFsIGF1dGhvciBvZiBtZDUuaCBpcyBMLlBldGVyIERldXRzY2ggICAgICAgIDxnaG9zdEBhbGFkZGluLmNvbT4uT3RoZXIgYXV0aG9ycyBhcmUgbm90ZWQgaW4gdGhlIGNoYW5nZSBoaXN0b3J5ICAgICAgICB0aGF0IGZvbGxvd3MoaW4gcmV2ZXJzZSBjaHJvbm9sb2dpY2FsIG9yZGVyKSA6ICAgICAgICAyMDAyIC0gMDQgLSAxMyBscGQgUmVtb3ZlZCBzdXBwb3J0IGZvciBub24gLSBBTlNJIGNvbXBpbGVyczsgcmVtb3ZlZCAgICAgICAgcmVmZXJlbmNlcyB0byBHaG9zdHNjcmlwdDsgY2xhcmlmaWVkIGRlcml2YXRpb24gZnJvbSBSRkMgMTMyMTsgICAgbm93IGhhbmRsZXMgYnl0ZSBvcmRlciBlaXRoZXIgc3RhdGljYWxseSBvciBkeW5hbWljYWxseS4gICAgICAgIDE5OTkgLSAxMSAtIDA0IGxwZCBFZGl0ZWQgY29tbWVudHMgc2xpZ2h0bHkgZm9yIGF1dG9tYXRpYyBUT0MgZXh0cmFjdGlvbi4gICAgICAgIDE5OTkgLSAxMCAtIDE4IGxwZCBGaXhlZCB0eXBvIGluIGhlYWRlciBjb21tZW50KGFuc2kya25yIHJhdGhlciB0aGFuIG1kNSk7ICAgIGFkZGVkIGNvbmRpdGlvbmFsaXphdGlvbiBmb3IgQysrIGNvbXBpbGF0aW9uIGZyb20gTWFydGluICAgICAgICBQdXJzY2hrZSA8cHVyc2Noa2VAYm5sLmdvdj4uICAgICAgICAxOTk5IC0gMDUgLSAwMyBscGQgT3JpZ2luYWwgdmVyc2lvbi4=");

    icodelens = B64_DESIZE(irtn);
    char *pbuf2 = new(std::nothrow) char[icodelens];
    irtn = b64decode(pbuf, irtn, pbuf2);
    CPPUNIT_ASSERT(std::string(pbuf2, irtn) == plmsg);
    SAFE_DELARR(pbuf);
    SAFE_DELARR(pbuf2);
}
void ctest_utils::test_str(void)
{
    PRINTF("%s", "test_str");
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

    str = "1ec47opasdtv\rF&*가";
    str2 = tohex(str.c_str(), str.size());
    CPPUNIT_ASSERT("31 65 63 34 37 6F 70 61 73 64 74 76 0D 46 26 2A B0 A1" == str2);

    str = " \t 가\nadc가 \r\n\t\v\t\t";
    std::vector<std::string> vrtn;
    split(str, "\t", vrtn);
    CPPUNIT_ASSERT(vrtn.size() == 5);
}
void ctest_utils::test_sock(void)
{
     PRINTF("%s", "test_sock");
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
void ctest_utils::test_loger(void)
{
    PRINTF("%s", "test_loger");
    int32_t icount = 0;
    for (int i = 0; i < 1; i++)
    {
        SETLOGLV(LOGLV_DEBUG);
        SETLOGPRT(true);
        printf("%s", "\n---------------------------------------------------------\n");
        LOGER_DEBUG("%s %d", "test debug", icount++);
        LOGER_INFO("%s %d", "test info", icount++);
        LOGER_WARN("%s %d", "test warn", icount++);
        LOGER_ERROR("%s %d", "test error", icount++);
        LOGER_FATAL("%s %d", "test fatal", icount++);

        MSLEEP(100);
        printf("%s", "-----------------Not have debug---------------------------\n");
        SETLOGLV(LOGLV_INFO);
        LOGER_DEBUG("%s %d", "test debug", icount++);
        LOGER_INFO("%s %d", "test info", icount++);
        LOGER_WARN("%s %d", "test warn", icount++);
        LOGER_ERROR("%s %d", "test error", icount++);
        LOGER_FATAL("%s %d", "test fatal", icount++);

        MSLEEP(100);
        printf("%s", "-----------------nothing print---------------------------\n");
        SETLOGPRT(false);
        LOGER_DEBUG("%s %d", "test debug", icount++);
        LOGER_INFO("%s %d", "test info", icount++);
        LOGER_WARN("%s %d", "test warn", icount++);
        LOGER_ERROR("%s %d", "test error", icount++);
        LOGER_FATAL("%s %d", "test fatal", icount++);
    }

    CPPUNIT_ASSERT(true);
}
