#include "test_utils.h"
#include "lib.h"
#include "proto/http.h"
#include "proto/simple.h"


ARRAY_DECL(int, arr);
void test_array(CuTest* tc) {
    arr arry;
    arr_init(&arry, 10);
    CuAssertTrue(tc, 0 == arr_size(&arry));
    CuAssertTrue(tc, 10 == arr_maxsize(&arry));
    CuAssertTrue(tc, arr_empty(&arry));

    for (int i = 1; i <= 20; i++) {
        arr_push_back(&arry, &i);
    }
    //arr_del_nomove(&arry, 0);
    //CuAssertTrue(tc, 20 == *arr_at(&arry, 0));
    CuAssertTrue(tc, 20 == arr_size(&arry));
    CuAssertTrue(tc, 20 == arr_maxsize(&arry));
    CuAssertTrue(tc, !arr_empty(&arry));

    CuAssertTrue(tc, 1 == *arr_at(&arry, 0) && 1 == *arr_front(&arry));
    CuAssertTrue(tc, 20 == *arr_at(&arry, 19) && 20 == *arr_back(&arry));

    arr_swap(&arry, 0, 19);
    CuAssertTrue(tc, 20 == *arr_at(&arry, 0) && 20 == *arr_front(&arry));
    CuAssertTrue(tc, 1 == *arr_at(&arry, 19) && 1 == *arr_back(&arry));
    arr_swap(&arry, 0, 19);

    arr_resize(&arry, 29);
    CuAssertTrue(tc, 30 == arr_maxsize(&arry));

    int val = 21;
    arr_add(&arry, &val, 20);
    CuAssertTrue(tc, 21 == *arr_at(&arry, 20) && 21 == *arr_back(&arry));
    val = 22;
    arr_add(&arry, &val, 20);
    CuAssertTrue(tc, 22 == *arr_at(&arry, 20));
    CuAssertTrue(tc, 21 == *arr_back(&arry));

    arr_del(&arry, 20);
    CuAssertTrue(tc, 21 == *arr_at(&arry, 20));
    CuAssertTrue(tc, 21 == *arr_pop_back(&arry));
    CuAssertTrue(tc, 20 == *arr_at(&arry, 19) && 20 == *arr_back(&arry));
    arr_del_nomove(&arry, 18);
    CuAssertTrue(tc, 20 == *arr_at(&arry, 18) && 20 == *arr_back(&arry));
    CuAssertTrue(tc, 20 == *arr_pop_back(&arry));
    for (size_t i = 0; i < arr_size(&arry); i++) {
        CuAssertTrue(tc, i + 1 == *arr_at(&arry, i));
    }

    arr_clear(&arry);
    CuAssertTrue(tc, 0 == arr_size(&arry));
    arr_resize(&arry, 10);
    CuAssertTrue(tc, 10 == arr_maxsize(&arry));
    CuAssertTrue(tc, NULL == arr_pop_back(&arry));

    arr_free(&arry);
}
QUEUE_DECL(int, que);
QUEUE_DECL(int *, qup);
void test_queue(CuTest* tc) {
    int test = 12;
    int *ptest = &test;
    qup qp;
    qup_init(&qp, 4);
    qup_push(&qp, &ptest);
    int *ppop = *qup_pop(&qp);
    CuAssertTrue(tc, *ppop == test);
    qup_free(&qp);

    que quseed;
    que_init(&quseed, 5);
    for (int i = 0; i < 5; i++) {
        que_push(&quseed, &i);
    }
    //0 1 2 3 4
    que_pop(&quseed);
    que_pop(&quseed);
    test = 5;
    que_push(&quseed, &test);
    test = 6;
    que_push(&quseed, &test);
    ptest = que_at(&quseed, que_size(&quseed));
    CuAssertTrue(tc, NULL == ptest);
    for (int i = 0; i < (int)que_size(&quseed); i++) {
        ptest = que_at(&quseed, i);
        CuAssertTrue(tc, *ptest == i + 2);
    }
    que_clear(&quseed);
    ptest = que_at(&quseed, 0);
    CuAssertTrue(tc, NULL == ptest);
    que_free(&quseed);

    que qu;
    que_init(&qu, 4);
    CuAssertTrue(tc, 0 == que_size(&qu));
    CuAssertTrue(tc, 4 == que_maxsize(&qu));
    CuAssertTrue(tc, que_empty(&qu));

    for (int i = 1; i <= 4; i++) {
        que_push(&qu, &i);
    }// 1 2 3 4
    CuAssertTrue(tc, 1 == *que_peek(&qu));
    CuAssertTrue(tc, 4 == que_size(&qu));
    CuAssertTrue(tc, 4 == que_maxsize(&qu));

    for (int i = 5; i <= 7; i++) {
        que_push(&qu, &i);
    }
    //1 2 3 4 5 6 7
    CuAssertTrue(tc, 7 == que_size(&qu));
    CuAssertTrue(tc, 8 == que_maxsize(&qu));

    int i = 8;
    que_push(&qu, &i);
    que_resize(&qu, 7);
    CuAssertTrue(tc, 8 == que_maxsize(&qu));
    //1 2 3 4 5 6 7 8

    for (int i = 1; i <= 6; i++) {
        CuAssertTrue(tc, i == *que_pop(&qu));
    }
    //1 2 3 4 5 6 [7 8]
    for (int i = 9; i <= 12; i++) {
        que_push(&qu, &i);
    }
    //9 0a 0b 0c .... 7 8
    que_resize(&qu, 6);
    //7 8 9 0a 0b 0c
    size_t size = que_size(&qu);
    for (size_t i = 0; i < size; i++) {
        CuAssertTrue(tc, i + 7 == *que_pop(&qu));
    }

    CuAssertTrue(tc, que_empty(&qu));
    CuAssertTrue(tc, NULL == que_pop(&qu));

    que_push(&qu, &i);
    que_clear(&qu);
    CuAssertTrue(tc, que_empty(&qu));
    CuAssertTrue(tc, 0 == que_size(&qu));
    
    que_free(&qu);
}
void test_system(CuTest* tc) {
    PRINT("createid: %"PRIu64"", createid());
    PRINT("threadid: %"PRIu64"", threadid());
    PRINT("procscnt: %d", procscnt());
    if (ERR_OK == bigendian()) {
        PRINT("big endian");
    }  else  {
        PRINT("little ndian");
    }    
    PRINT("procscnt: %d", procscnt());
    const char *path = procpath();
    PRINT("procpath: %s", path);
    CuAssertTrue(tc, ERR_OK == isdir(path));
    CuAssertTrue(tc, ERR_OK != isfile(path));

    const char * skp1 = "     ";
    char *pos = skipempty(skp1, strlen(skp1));
    CuAssertTrue(tc, NULL == pos);
    const char * skp2 = "    1 ";
    pos = skipempty(skp2, strlen(skp2));
    CuAssertTrue(tc, skp2 + 4 == pos);
    const char * skp3 = "1    1 ";
    pos = skipempty(skp3, strlen(skp3));
    CuAssertTrue(tc, skp3 == pos);
    
    const char *ptr1 = "this is test";
    pos = memstr(0, ptr1, strlen(ptr1), "this", strlen("this"));
    CuAssertTrue(tc, pos == ptr1);
    pos = memstr(0, ptr1, strlen(ptr1), "tE", strlen("tE"));
    CuAssertTrue(tc, pos == NULL);
    pos = memstr(0, ptr1, strlen(ptr1), "te", strlen("te"));
    CuAssertTrue(tc, pos == ptr1 + 8);
    pos = memstr(0, ptr1, strlen(ptr1), "test", strlen("test"));
    CuAssertTrue(tc, pos == ptr1 + 8);
    pos = memstr(0, ptr1, strlen(ptr1), "test1", strlen("test1"));
    CuAssertTrue(tc, pos == NULL);
    pos = memstr(1, ptr1, strlen(ptr1), "thIs", strlen("thIs"));
    CuAssertTrue(tc, pos == ptr1);
    pos = memstr(1, ptr1, strlen(ptr1), "tE", strlen("tE"));
    CuAssertTrue(tc, pos == ptr1 + 8);
    pos = memstr(1, ptr1, strlen(ptr1), "teSt", strlen("teSt"));
    CuAssertTrue(tc, pos == ptr1 + 8);
    pos = memstr(1, ptr1, strlen(ptr1), "test1", strlen("test1"));
    CuAssertTrue(tc, pos == NULL);  

    struct timeval tv;
    timeofday(&tv);
    PRINT("timeofday tv_sec: %d tv_usec: %d", (uint32_t)tv.tv_sec, (uint32_t)tv.tv_usec);
    PRINT("nowsec: %"PRIu64"", nowsec());
    PRINT("nowms: %"PRIu64"", nowms());
    char time[TIME_LENS] = { 0 };
    nowtime("%Y-%m-%d %H:%M:%S", time);
    PRINT("nowtime: %s", time);
    nowmtime("%Y-%m-%d %H:%M:%S", time);
    PRINT("nowmtime: %s", time);

    const char *str = "this is test.";
    size_t len = strlen(str);
    CuAssertTrue(tc, 0x7610 == crc16(str, len));
    CuAssertTrue(tc, 0x3B610CF9 == crc32(str, len));

    unsigned char sha1str[20];
    sha1_ctx sha1;
    sha1_init(&sha1);
    sha1_update(&sha1, (unsigned char*)str, len);
    sha1_final(&sha1, sha1str);
    char out[HEX_ENSIZE(20)];
    tohex(sha1str, sizeof(sha1str), out);
    CuAssertTrue(tc, 0 == strcmp("F1B188A879C1C82D561CB8A064D825FDCBFE4191", out));

    unsigned char sh256[32];
    sha256_ctx sha256;
    sha256_init(&sha256);
    sha256_update(&sha256, (unsigned char*)str, len);
    sha256_final(&sha256, sh256);
    char osh256[HEX_ENSIZE(32)];
    tohex(sh256, sizeof(sh256), osh256);
    CuAssertTrue(tc, 0 == strcmp(osh256, "FECC75FE2A23D8EAFBA452EE0B8B6B56BECCF52278BF1398AADDEECFE0EA0FCE"));

    unsigned char md5str[16];
    md5_ctx md5;
    md5_init(&md5);  
    md5_update(&md5, (unsigned char*)str, len);
    md5_final(&md5, md5str);
    char omd5str[HEX_ENSIZE(16)];
    tohex(md5str, sizeof(md5str), omd5str);
    CuAssertTrue(tc, 0 == strcmp("480FC0D368462326386DA7BB8ED56AD7", omd5str));

    char *en;
    MALLOC(en, B64_ENSIZE(len));
    b64_encode(str, len, en);
    CuAssertTrue(tc, 0 == strcmp("dGhpcyBpcyB0ZXN0Lg==", en));
    char *de;
    MALLOC(de, B64_DESIZE(strlen(en)));
    size_t bdelen = b64_decode(en, strlen(en), de);
    CuAssertTrue(tc, 0 == strcmp(de, str));
    FREE(en);

    char key[4] = { 1, 5 ,25, 120 };
    xor_encode(key, 1, de, len);
    xor_decode(key, 1, de, bdelen);
    CuAssertTrue(tc, 0 == strcmp(de, str));
    FREE(de);

    uint64_t hs = hash(str, len);
    CuAssertTrue(tc, 14869103789476489700ULL == hs);

    const char *url = "this is URL参数编码 test #@.";
    char *enurl;
    MALLOC(enurl, URL_ENSIZE(strlen(url)));
    url_encode(url, strlen(url), enurl);
    url_decode(enurl, strlen(enurl));
    CuAssertTrue(tc, 0 == strcmp(url, enurl));
    FREE(enurl);

    char *buf;
    CALLOC(buf, 1, (size_t)32);
    memcpy(buf, str, strlen(str));
    CuAssertTrue(tc, 0 == strcmp(strupper(buf), "THIS IS TEST."));
    CuAssertTrue(tc, 0 == strcmp(strlower(buf), "this is test."));
    CuAssertTrue(tc, 0 == strcmp(strreverse(buf), ".tset si siht"));
    FREE(buf);

    PRINT("randrange: %d", randrange(0, 100));
    PRINT("randrange: %d", randrange(0, 100));
    PRINT("randrange: %d", randrange(0, 100));
    char rdbuf[64] = { 0 };
    randstr(rdbuf, sizeof(rdbuf) - 1);
    PRINT("randstr: %s", rdbuf);

    char *fmt = formatv("%d-%s", 110, "come");
    CuAssertTrue(tc, 0 == strcmp(fmt, "110-come"));
    FREE(fmt);
}
void test_timer(CuTest* tc) {
    timer_ctx timer;
    timer_init(&timer);
    timer_start(&timer);
    MSLEEP(100);
    PRINT("timer_elapsed_ms: %"PRIu64"", timer_elapsed_ms(&timer));
}
void test_netutils(CuTest* tc) {
    sock_init();
    SOCKET sock[2];
    CuAssertTrue(tc, ERR_OK == sock_pair(sock));
    CuAssertTrue(tc, SOCK_STREAM == sock_type(sock[0]));
    CuAssertTrue(tc, AF_INET == sock_family(sock[0]));
    const char *str = "this is test.";
    send(sock[0], str, (int)strlen(str), 0);
    MSLEEP(50);
    size_t nread = sock_nread(sock[1]);
    CuAssertTrue(tc, strlen(str) == nread);
    CLOSE_SOCK(sock[0]);
    CLOSE_SOCK(sock[1]);
    sock_clean();
}
void test_buffer(CuTest* tc) {
    buffer_ctx buf;
    buffer_init(&buf); 
    const char *str1 = "this is test.";
    const char *str2 = "who am i?";
    CuAssertTrue(tc, ERR_OK == buffer_append(&buf, (void *)str1, strlen(str1)));
    CuAssertTrue(tc, ERR_OK == buffer_appendv(&buf, "%s", str2));

    CuAssertTrue(tc, ERR_FAILED == buffer_search(&buf, 1, 12, 13, "t.W", strlen("t.W")));
    CuAssertTrue(tc, ERR_FAILED == buffer_search(&buf, 1, 0, 12, "t.W", strlen("t.W")));
    CuAssertTrue(tc, 11 == buffer_search(&buf, 1, 0, 13, "t.W", strlen("t.W")));

    CuAssertTrue(tc, ERR_FAILED == buffer_search(&buf, 1, 0, 12, "Who", strlen("Who")));
    CuAssertTrue(tc, ERR_FAILED == buffer_search(&buf, 1, 0, 14, "Who", strlen("Who")));
    CuAssertTrue(tc, 13 == buffer_search(&buf, 1, 0, 15, "Who", strlen("Who")));

    CuAssertTrue(tc, ERR_FAILED == buffer_search(&buf, 0, 12, 13, "t.w", strlen("t.w")));
    CuAssertTrue(tc, ERR_FAILED == buffer_search(&buf, 0, 0, 12, "t.w", strlen("t.w")));
    CuAssertTrue(tc, ERR_FAILED == buffer_search(&buf, 0, 0, 13, "t.W", strlen("t.W")));
    CuAssertTrue(tc, 11 == buffer_search(&buf, 0, 0, 13, "t.w", strlen("t.w")));

    CuAssertTrue(tc, ERR_FAILED == buffer_search(&buf, 0, 0, 12, "who", strlen("who")));
    CuAssertTrue(tc, ERR_FAILED == buffer_search(&buf, 0, 0, 14, "who", strlen("who")));
    CuAssertTrue(tc, ERR_FAILED == buffer_search(&buf, 0, 0, 15, "Who", strlen("Who")));
    CuAssertTrue(tc, 13 == buffer_search(&buf, 0, 0, 15, "who", strlen("who")));

    CuAssertTrue(tc, strlen(str1) + strlen(str2) == buffer_size(&buf));
    char out[ONEK] = { 0 };
    int32_t rtn = buffer_copyout(&buf, 0, out, strlen(str1));
    CuAssertTrue(tc, rtn == strlen(str1) && 0 == strcmp(str1, out));
    rtn = buffer_drain(&buf, rtn);
    CuAssertTrue(tc, rtn == strlen(str1) && strlen(str2) == buffer_size(&buf));
    ZERO(out, sizeof(out));
    rtn = buffer_remove(&buf, out, buffer_size(&buf));
    CuAssertTrue(tc, rtn == strlen(str2) && 0 == strcmp(str2, out));

    buffer_free(&buf);
}
void test_log(CuTest* tc) {
    LOG_DEBUG("%s", "LOG_DEBUG");
    LOG_INFO("%s", "LOG_INFO");
    LOG_WARN("%s", "LOG_WARN");
    LOG_ERROR("%s", "LOG_ERROR");
}
void test_http(CuTest* tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    size_t size = 0;
    int32_t closed = 0;
    int32_t slice;
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = PACK_HTTP;
    const char *http1 = "POST /users HTTP/1.1\r\n  Host:   api.github.com\r\nContent-Length: 5\r\na: \r\n\r\n1";
    buffer_append(&buf, (void *)http1, strlen(http1));
    void *rtnbuf = http_unpack(&buf, &size, &ud, &closed, &slice);
    CuAssertTrue(tc, NULL == rtnbuf);
    const char *http2 = "2345";
    buffer_append(&buf, (void *)http2, strlen(http2));
    rtnbuf = http_unpack(&buf, &size, &ud, &closed, &slice);
    CuAssertTrue(tc, NULL != rtnbuf);
    protos_pkfree(PACK_HTTP, rtnbuf);

    const char *http3 = "POST /users HTTP/1.1\r\nHost: api.github.com\r\nContent-Length: 5\r\n\r\n12345";
    buffer_append(&buf, (void *)http3, strlen(http3));
    rtnbuf = http_unpack(&buf, &size, &ud, &closed, &slice);
    CuAssertTrue(tc, NULL != rtnbuf);
    protos_pkfree(PACK_HTTP, rtnbuf);

    const char *http4 = "POST /users HTTP/1.1\r\nHost: api.github.com\r\n\r\n";
    buffer_append(&buf, (void *)http4, strlen(http4));
    rtnbuf = http_unpack(&buf, &size, &ud, &closed, &slice);
    CuAssertTrue(tc, NULL != rtnbuf);
    protos_pkfree(PACK_HTTP, rtnbuf);

    const char *http5 = "POST /users HTTP/1.1\r\nHost: api.github.com\r\nTransfer-Encoding: chunked\r\n\r\n7\r\nMozilla\r\nb\r\nDeveloper N\r\n0\r\n\r\n";
    buffer_append(&buf, (void *)http5, strlen(http5));
    rtnbuf = http_unpack(&buf, &size, &ud, &closed, &slice);
    CuAssertTrue(tc, NULL != rtnbuf && SLICE_START == slice);
    protos_pkfree(PACK_HTTP, rtnbuf);
    rtnbuf = http_unpack(&buf, &size, &ud, &closed, &slice);
    CuAssertTrue(tc, NULL != rtnbuf && SLICE == slice);
    protos_pkfree(PACK_HTTP, rtnbuf);
    rtnbuf = http_unpack(&buf, &size, &ud, &closed, &slice);
    CuAssertTrue(tc, NULL != rtnbuf && SLICE == slice);
    protos_pkfree(PACK_HTTP, rtnbuf);
    rtnbuf = http_unpack(&buf, &size, &ud, &closed, &slice);
    CuAssertTrue(tc, NULL != rtnbuf && SLICE_END == slice);
    protos_pkfree(PACK_HTTP, rtnbuf);
    protos_udfree(&ud);

    buffer_free(&buf);
}
void test_url(CuTest* tc) {
    //[协议类型]://[访问资源需要的凭证信息]@[服务器地址]:[端口号]/[资源层级UNIX文件路径][文件名]?[查询]#[片段ID]
    url_ctx url;
    const char *url1 = "ftp://user:psw@127.0.0.1:8080/path/file?a=1&b=2#anchor";
    url_parse(&url, (char *)url1, strlen(url1));
    CuAssertTrue(tc, 
        NULL != url.scheme.data
        && NULL != url.user.data
        && NULL != url.psw.data
        && NULL != url.host.data
        && NULL != url.port.data
        && NULL != url.path.data
        && NULL != url.anchor.data
        && NULL != url.param[0].key.data
        && NULL != url.param[1].key.data);
    const char *url2 = "ftp://user@127.0.0.1/path?c=#anchor";
    url_parse(&url, (char *)url2, strlen(url2));
    CuAssertTrue(tc,
        NULL != url.scheme.data
        && NULL != url.user.data
        && NULL == url.psw.data
        && NULL != url.host.data
        && NULL == url.port.data
        && NULL != url.path.data
        && NULL != url.anchor.data
        && NULL != url.param[0].key.data);
    const char *url3 = "ftp://127.0.0.1/?c=";
    url_parse(&url, (char *)url3, strlen(url3));
    CuAssertTrue(tc,
        NULL != url.scheme.data
        && NULL == url.user.data
        && NULL == url.psw.data
        && NULL != url.host.data
        && NULL == url.port.data
        && NULL == url.path.data
        && NULL == url.anchor.data
        && NULL != url.param[0].key.data);
    const char *url4 = "ftp://127.0.0.1/?c=#";
    url_parse(&url, (char *)url4, strlen(url4));
    CuAssertTrue(tc,
        NULL != url.scheme.data
        && NULL == url.user.data
        && NULL == url.psw.data
        && NULL != url.host.data
        && NULL == url.port.data
        && NULL == url.path.data
        && NULL == url.anchor.data
        && NULL != url.param[0].key.data);
    const char *url5 = "ftp://127.0.0.1/?#";
    url_parse(&url, (char *)url5, strlen(url5));
    CuAssertTrue(tc,
        NULL != url.scheme.data
        && NULL == url.user.data
        && NULL == url.psw.data
        && NULL != url.host.data
        && NULL == url.port.data
        && NULL == url.path.data
        && NULL == url.anchor.data
        && NULL == url.param[0].key.data);
    const char *url6 = "127.0.0.1/?a=1";
    url_parse(&url, (char *)url6, strlen(url6));
    CuAssertTrue(tc,
        NULL == url.scheme.data
        && NULL == url.user.data
        && NULL == url.psw.data
        && NULL != url.host.data
        && NULL == url.port.data
        && NULL == url.path.data
        && NULL == url.anchor.data
        && NULL != url.param[0].key.data);
    const char *url7 = "127.0.0.1/#1";
    url_parse(&url, (char *)url7, strlen(url7));
    CuAssertTrue(tc,
        NULL == url.scheme.data
        && NULL == url.user.data
        && NULL == url.psw.data
        && NULL != url.host.data
        && NULL == url.port.data
        && NULL == url.path.data
        && NULL != url.anchor.data
        && NULL == url.param[0].key.data);
    const char *url8 = "/path?a=1";
    url_parse(&url, (char *)url8, strlen(url8));
    CuAssertTrue(tc,
        NULL == url.scheme.data
        && NULL == url.user.data
        && NULL == url.psw.data
        && NULL == url.host.data
        && NULL == url.port.data
        && NULL != url.path.data
        && NULL == url.anchor.data
        && NULL != url.param[0].key.data);
    const char *url9 = "/path#12";
    url_parse(&url, (char *)url9, strlen(url9));
    CuAssertTrue(tc,
        NULL == url.scheme.data
        && NULL == url.user.data
        && NULL == url.psw.data
        && NULL == url.host.data
        && NULL == url.port.data
        && NULL != url.path.data
        && NULL != url.anchor.data
        && NULL == url.param[0].key.data);
    const char *url10 = "/path";
    url_parse(&url, (char *)url10, strlen(url10));
    CuAssertTrue(tc,
        NULL == url.scheme.data
        && NULL == url.user.data
        && NULL == url.psw.data
        && NULL == url.host.data
        && NULL == url.port.data
        && NULL != url.path.data
        && NULL == url.anchor.data
        && NULL == url.param[0].key.data);
    const char *url11 = "127.0.0.1/path#anchor";
    url_parse(&url, (char *)url11, strlen(url11));
    CuAssertTrue(tc,
        NULL == url.scheme.data
        && NULL == url.user.data
        && NULL == url.psw.data
        && NULL != url.host.data
        && NULL == url.port.data
        && NULL != url.path.data
        && NULL != url.anchor.data
        && NULL == url.param[0].key.data);
    const char *url12 = "ftp://127.0.0.1";
    url_parse(&url, (char *)url12, strlen(url12));
    CuAssertTrue(tc,
        NULL != url.scheme.data
        && NULL == url.user.data
        && NULL == url.psw.data
        && NULL != url.host.data
        && NULL == url.port.data
        && NULL == url.path.data
        && NULL == url.anchor.data
        && NULL == url.param[0].key.data);
    const char *url13 = "ftp://";
    url_parse(&url, (char *)url13, strlen(url13));
    CuAssertTrue(tc,
        NULL != url.scheme.data
        && NULL == url.user.data
        && NULL == url.psw.data
        && NULL == url.host.data
        && NULL == url.port.data
        && NULL == url.path.data
        && NULL == url.anchor.data
        && NULL == url.param[0].key.data);
    const char *url14 = "ws://127.0.0.1:15003";
    url_parse(&url, (char *)url14, strlen(url14));
    CuAssertTrue(tc,
        NULL != url.scheme.data
        && NULL == url.user.data
        && NULL == url.psw.data
        && NULL != url.host.data
        && NULL != url.port.data
        && NULL == url.path.data
        && NULL == url.anchor.data
        && NULL == url.param[0].key.data);
}
void test_utils(CuSuite* suite) {
    SUITE_ADD_TEST(suite, test_array);
    SUITE_ADD_TEST(suite, test_queue);
    SUITE_ADD_TEST(suite, test_system);
    SUITE_ADD_TEST(suite, test_timer);
    SUITE_ADD_TEST(suite, test_netutils);
    SUITE_ADD_TEST(suite, test_buffer);
    SUITE_ADD_TEST(suite, test_log);
    SUITE_ADD_TEST(suite, test_http);
    SUITE_ADD_TEST(suite, test_url);
}
