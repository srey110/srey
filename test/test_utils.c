#include "test_utils.h"
#include "lib.h"
#include "protocol/http.h"
#include "protocol/custz.h"

ARRAY_DECL(int, arr);
static void test_array(CuTest* tc) {
    arr_ctx arry;
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
    for (uint32_t i = 0; i < arr_size(&arry); i++) {
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
static void test_queue(CuTest* tc) {
    int test = 12;
    int *ptest = &test;
    qup_ctx qp;
    qup_init(&qp, 4);
    qup_push(&qp, &ptest);
    int *ppop = *qup_pop(&qp);
    CuAssertTrue(tc, *ppop == test);
    qup_free(&qp);

    que_ctx quseed;
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

    que_ctx qu;
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
typedef struct heap_test {
    heap_node node;
    int32_t index;
}heap_test;
static int _heap_test_compare(const heap_node* lhs, const heap_node* rhs) {
    return UPCAST(lhs, heap_test, node)->index < UPCAST(rhs, heap_test, node)->index;
}
static void test_heap(CuTest* tc) {
    heap_ctx heap;
    heap_init(&heap, _heap_test_compare);
    heap_test ht[10];
    ZERO(ht, sizeof(ht));
    for (int32_t i = 0; i < 10; i++) {
        ht[i].index = i + 1;
        heap_insert(&heap, &ht[i].node);
    }
    heap_remove(&heap, &ht[1].node);
    int32_t iVal;
    for (int32_t i = 1; i <= 9; i++) {
        iVal = UPCAST(heap.root, heap_test, node)->index;
        if (1 == i) {
            CuAssertTrue(tc, i == iVal);
        } else {
            CuAssertTrue(tc, i + 1 == iVal);
        }
        heap_dequeue(&heap);
    }
}
static void test_system(CuTest* tc) {
    PRINT("createid: %"PRIu64"", createid());
    PRINT("threadid: %"PRIu64"", threadid());
    PRINT("procscnt: %d", procscnt());
    if (!is_little()) {
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

    size_t n;
    char tmp[20];
    const char *spstr = " this  is test ";
    buf_ctx *spbuf = split(spstr, strlen(spstr), "  ", 1, &n);
    CuAssertTrue(tc, 6 == n);
    for (size_t i = 0; i < n; i++) {
        if (NULL != spbuf[i].data) {
            ZERO(tmp, 20);
            memcpy(tmp, spbuf[i].data, spbuf[i].lens);
            PRINT("%s", tmp);
        } else {
            PRINT("%s", "NULL");
        }
    }
    FREE(spbuf);
    sfid_ctx sfid;
    sfid_init(&sfid, 101, 0, 0, 0);
    uint64_t id = sfid_id(&sfid);
    PRINT("sfid_id: %"PRIu64"", id);
    uint64_t timestamp;
    int32_t machineid;
    int32_t sequence;
    sfid_decode(&sfid, id, &timestamp, &machineid, &sequence);  
    CuAssertTrue(tc, 101 == machineid);

    int32_t tz = timeoffset() / 60;
    PRINT("timeoffset: %d", tz);
    struct timeval tv;
    timeofday(&tv);
    PRINT("timeofday tv_sec: %d tv_usec: %d", (uint32_t)tv.tv_sec, (uint32_t)tv.tv_usec);
    PRINT("nowsec: %"PRIu64"", nowsec());
    PRINT("nowms: %"PRIu64"", nowms());
    char time[TIME_LENS] = { 0 };
    mstostr(nowms(), "%Y-%m-%d %H:%M:%S", time);
    PRINT("nowmtime: %s", time);
    timestamp = nowsec();
    sectostr(timestamp, "%Y-%m-%d %H:%M:%S", time);
    PRINT("nowtime: %s", time);
    PRINT("%s", "test strtots");
    CuAssertTrue(tc, timestamp == strtots(time, "%Y-%m-%d %H:%M:%S"));
    PRINT("%s", "test strtots end");
   
    const char *str = "this is test.";
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

    char *fmt = format_va("%d-%s", 110, "come");
    CuAssertTrue(tc, 0 == strcmp(fmt, "110-come"));
    FREE(fmt);
}
static void test_crypt_other(CuTest* tc) {
    const char *str = "RHdzNjc1OFhHS1dHME9LMUxOTEQySDREUTRz5Lit5paHRkNTVWRaOFJKRkxvNk9YYlBmaDRkYm1TQUNZaU1VMHBQeFU0NGI";
    size_t len = strlen(str);
    CuAssertTrue(tc, 0xA6F5 == crc16(str, len));
    CuAssertTrue(tc, 0xFCD68BE4 == crc32(str, len));

    char *en;
    MALLOC(en, B64EN_SIZE(len));
    bs64_encode(str, len, en);
    CuAssertTrue(tc, 0 == strcmp("Ukhkek5qYzFPRmhIUzFkSE1FOUxNVXhPVEVReVNEUkVVVFJ6NUxpdDVwYUhSa05UVldSYU9GSktSa3h2Tms5WVlsQm1hRFJrWW0xVFFVTlphVTFWTUhCUWVGVTBOR0k=", en));
    char *de;
    MALLOC(de, B64DE_SIZE(strlen(en)));
    size_t bdelen = bs64_decode(en, strlen(en), de);
    CuAssertTrue(tc, 0 == strcmp(de, str));
    const char *en2 = "\r\nUkhkek5qYzFPRmhIUzFkSE1FOUxNVXhPVEVRe\r\nVNEUkVVVFJ6NUxpdDVwYUhSa05UVldSYU9GSktSa3h2Tms5WVls\nQm1hRFJrWW0xVFFVTlphVTFWTUhCUWVGVTBOR0k\r\n=";
    bdelen = bs64_decode(en2, strlen(en2), de);
    CuAssertTrue(tc, 0 == strcmp(de, str));
    FREE(en);

    char key[4] = { 1, 5 ,25, 120 };
    xor_encode(key, 1, de, len);
    xor_decode(key, 1, de, bdelen);
    CuAssertTrue(tc, 0 == strcmp(de, str));
    FREE(de);

    uint64_t hs = hash(str, len);
    CuAssertTrue(tc, 5232889973870020308ULL == hs);

    const char *url = "this is URL参数编码 test #@.";
    char *enurl;
    len = strlen(url);
    MALLOC(enurl, URLEN_SIZE(len));
    url_encode(url, len, enurl);
    len = url_decode(enurl, strlen(enurl));
    CuAssertTrue(tc, 0 == strcmp(url, enurl));
    FREE(enurl);
}
static void test_digest(CuTest* tc) {
    const char *str = "RHdzNjc1OFhHS1dHME9LMUxOTEQySDREUTRz5Lit5paHRkNTVWRaOFJKRkxvNk9YYlBmaDRkYm1TQUNZaU1VMHBQeFU0NGI";
    size_t len = strlen(str);
    digest_ctx digest;
    char hsbuf[DG_BLOCK_SIZE];
    char out[HEX_ENSIZE(DG_BLOCK_SIZE)];
    digest_init(&digest, DG_SHA1);
    digest_update(&digest, "RHdzNjc1OFhHS1dHME9LMUxOTEQySDREUTRz5Lit5paHRkNTVWRaOFJKRkxvNk", strlen("RHdzNjc1OFhHS1dHME9LMUxOTEQySDREUTRz5Lit5paHRkNTVWRaOFJKRkxvNk"));
    digest_update(&digest, "9YYlBmaDRkYm1TQUNZaU1VMHBQeFU0NGI", strlen("9YYlBmaDRkYm1TQUNZaU1VMHBQeFU0NGI"));
    digest_final(&digest, hsbuf);
    tohex(hsbuf, digest_size(&digest), out);
    CuAssertTrue(tc, 0 == strcmp("8AE1EA68BC319E9BD0B55CBD93E4B2BCDCEF11A0", out));

    digest_init(&digest, DG_SHA256);
    digest_update(&digest, str, len);
    digest_final(&digest, hsbuf);
    tohex(hsbuf, digest_size(&digest), out);
    CuAssertTrue(tc, 0 == strcmp(out, "93AADF88D01C0D64B3376017DD5B2007CD51C04F0FF9BFA95A76E9319E4E428E"));
    digest_reset(&digest);
    digest_update(&digest, str, len);
    digest_update(&digest, str, len);
    digest_final(&digest, hsbuf);
    tohex(hsbuf, digest_size(&digest), out);
    CuAssertTrue(tc, 0 == strcmp(out, "60081338233A2372D5C0599E1335F860B45CC28905D1C9B9C4C6C6E5BC935FCE"));

    digest_init(&digest, DG_SHA512);
    digest_update(&digest, str, len);
    digest_update(&digest, str, len);
    digest_final(&digest, hsbuf);
    tohex(hsbuf, digest_size(&digest), out);
    CuAssertTrue(tc, 0 == strcmp(out, "E241F4163F124BA5C91505A60683D749F2BE0940597315D414CAA6137EA5C60E0B1446F8DA736A9B3DFAE9CE4D82905384EA6126DFE389AACE97485A13F22158"));

    digest_init(&digest, DG_MD2);
    digest_update(&digest, str, len);
    digest_final(&digest, hsbuf);
    tohex(hsbuf, digest_size(&digest), out);
    CuAssertTrue(tc, 0 == strcmp(out, "2E3929E8835E1359F0E9B3436B09F564"));

    digest_init(&digest, DG_MD4);
    digest_update(&digest, str, len);
    digest_final(&digest, hsbuf);
    tohex(hsbuf, digest_size(&digest), out);
    CuAssertTrue(tc, 0 == strcmp(out, "17FD8C05936B20BD8678FDC8D7C60FA5"));

    digest_init(&digest, DG_MD5);
    digest_update(&digest, str, len);
    digest_final(&digest, hsbuf);
    tohex(hsbuf, digest_size(&digest), out);
    CuAssertTrue(tc, 0 == strcmp("EB8CE1674B09464492A4CE35C38E89CE", out));

    const char *hmac_key[] = { "n3iDbIxJ79GaNxAfjTbSims0nQnEH131RnmQYZ6ofxoOn3bvVGBN45lcozfguAJl", //== 64
        "Dws6758XGKWG0OK1LNLD2H4DQ4sFCSUdZ8RJFLo6OXbPfh4dbmSACYiMU0pPxU44b",// > 64
        "Shm3lWMaDIxwKH64" };
    const char *hmac_md5_result[] = { "8A2A097E491D02E96383D63B7EFC97F5",
        "139100334F4EE0C4D9D506457F4811C7",
        "24E9A3D055F2E1984D78E52F7FDF03E3" };
    hmac_ctx macmd5;
    char outmmd5[MD5_BLOCK_SIZE];
    char hexmm5[HEX_ENSIZE(sizeof(outmmd5))];
    for (int32_t i = 0; i < 3; i++) {
        hmac_init(&macmd5, DG_MD5, hmac_key[i], strlen(hmac_key[i]));
        hmac_update(&macmd5, str, len);
        hmac_final(&macmd5, outmmd5);
        tohex(outmmd5, sizeof(outmmd5), hexmm5);
        CuAssertTrue(tc, 0 == strcmp(hmac_md5_result[i], hexmm5));
    }

    const char *hmac_sha1_result[] = { "451A902233ADDAF949F696D9333F0FE73B2B126E",
        "EA2A165A590F4D2A276CAD63D17E7FD004E0232C",
        "0BE13599959E0F24902E9840B992A75231F071A2" };
    hmac_ctx macsha1;
    char outmsha1[SHA1_BLOCK_SIZE];
    char hexmsha1[HEX_ENSIZE(sizeof(outmsha1))];
    for (int32_t i = 0; i < 3; i++) {
        hmac_init(&macsha1, DG_SHA1, hmac_key[i], strlen(hmac_key[i]));
        hmac_update(&macsha1, str, len);
        hmac_final(&macsha1, outmsha1);
        tohex(outmsha1, sizeof(outmsha1), hexmsha1);
        CuAssertTrue(tc, 0 == strcmp(hmac_sha1_result[i], hexmsha1));
    }

    const char *hmac_sha256_result[] = { "0E4A35BCF8D7466C5AA146AF1F70DB395B87B2176567503FAD27CB669D686174",
        "31F7613A4FDBF36EEC435976550576282D0C319632DAE09A94A296068DC8E017",
        "E5F25394FC77487B3C930ADDE609D260FFF722FFCE64E1778DD0EC4C3768918A" };
    hmac_ctx macsha256;
    char outmsha256[SHA256_BLOCK_SIZE];
    char hexmsha256[HEX_ENSIZE(sizeof(outmsha256))];
    for (int32_t i = 0; i < 3; i++) {
        hmac_init(&macsha256, DG_SHA256, hmac_key[i], strlen(hmac_key[i]));
        hmac_update(&macsha256, str, len);
        hmac_final(&macsha256, outmsha256);
        tohex(outmsha256, sizeof(outmsha256), hexmsha256);
        CuAssertTrue(tc, 0 == strcmp(hmac_sha256_result[i], hexmsha256));
    }
    hmac_reset(&macsha256);
    hmac_update(&macsha256, str, len);
    hmac_final(&macsha256, outmsha256);
    tohex(outmsha256, sizeof(outmsha256), hexmsha256);
    CuAssertTrue(tc, 0 == strcmp(hmac_sha256_result[2], hexmsha256));
}
static void test_cipher_ecb(CuTest* tc) {
    //PADDING_NONE
    char hex[HEX_ENSIZE(16)];
    size_t size;
    cipher_ctx cipher;
    cipher_init(&cipher, DES, ECB, "bOcUw4DP", strlen("bOcUw4DP"), 0, 1);
    size_t bklens = cipher_size(&cipher);
    void *out = cipher_block(&cipher, "12345678", bklens, &size);
    tohex(out, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "721DD0760AF558FF"));
    cipher_init(&cipher, DES, ECB, "bOcUw4DP", strlen("bOcUw4DP"), 0, 0);
    out = cipher_block(&cipher, out, bklens, &size);
    CuAssertTrue(tc, 0 == memcmp(out, "12345678", bklens));

    cipher_init(&cipher, DES3, ECB, "bOcUw4DPDbRJjEQXNVXhPVEV", strlen("bOcUw4DPDbRJjEQXNVXhPVEV"), 0, 1);
    bklens = cipher_size(&cipher);
    out = cipher_block(&cipher, "12345678", bklens, &size);
    tohex(out, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "B0A36B7AC8F9DBD1"));
    cipher_init(&cipher, DES3, ECB, "bOcUw4DPDbRJjEQXNVXhPVEV", strlen("bOcUw4DPDbRJjEQXNVXhPVEV"), 0, 0);
    out = cipher_block(&cipher, out, bklens, &size);
    CuAssertTrue(tc, 0 == memcmp(out, "12345678", bklens));

    cipher_init(&cipher, AES, ECB, "bPeu8B1FX2Bq", strlen("bPeu8B1FX2Bq"), 128, 1);
    bklens = cipher_size(&cipher);
    out = cipher_block(&cipher, "1234567812345678", bklens, &size);
    tohex(out, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "BFFD98F3FD59E7068007DC42E65D512D"));
    cipher_init(&cipher, AES, ECB, "bPeu8B1FX2Bq", strlen("bPeu8B1FX2Bq"), 128, 0);
    out = cipher_block(&cipher, out, bklens, &size);
    CuAssertTrue(tc, 0 == memcmp(out, "1234567812345678", bklens));

    cipher_init(&cipher, AES, ECB, "DbRJjEQXYDhBkEWWeOuCeaR3", strlen("DbRJjEQXYDhBkEWWeOuCeaR3"), 192, 1);
    out = cipher_block(&cipher, "1234567812345678", bklens, &size);
    tohex(out, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "389C7CBDCDDF02EB96254E102383ABD0"));
    cipher_init(&cipher, AES, ECB, "DbRJjEQXYDhBkEWWeOuCeaR3", strlen("DbRJjEQXYDhBkEWWeOuCeaR3"), 192, 0);
    out = cipher_block(&cipher, out, bklens, &size);
    CuAssertTrue(tc, 0 == memcmp(out, "1234567812345678", bklens));

    cipher_init(&cipher, AES, ECB, "cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC", strlen("cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC"), 256, 1);
    out = cipher_block(&cipher, "1234567812345678", bklens, &size);
    tohex(out, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "99FCBC57EEB5B54178494ACDCA16D3B5"));
    cipher_init(&cipher, AES, ECB, "cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC", strlen("cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC"), 256, 0);
    out = cipher_block(&cipher, out, bklens, &size);
    CuAssertTrue(tc, 0 == memcmp(out, "1234567812345678", bklens));

    cipher_init(&cipher, AES, ECB, "cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC", strlen("cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC"), 256, 1);
    cipher_padding(&cipher, ZeroPadding);
    out = cipher_block(&cipher, "12345678", 8, &size);
    tohex(out, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "10D97A9B601556D63B19A5FE927527EE"));
    cipher_init(&cipher, AES, ECB, "cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC", strlen("cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC"), 256, 0);
    cipher_padding(&cipher, ZeroPadding);
    out = cipher_block(&cipher, out, bklens, &size);
    CuAssertTrue(tc, 0 == memcmp(out, "12345678", 8));

    cipher_init(&cipher, AES, ECB, "cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC", strlen("cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC"), 256, 1);
    cipher_padding(&cipher, PKCS57);
    out = cipher_block(&cipher, "12345678", 8, &size);
    tohex(out, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "F1F0FD6E3DE1F7121592B3705C9F2F4B"));
    cipher_init(&cipher, AES, ECB, "cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC", strlen("cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC"), 256, 0);
    cipher_padding(&cipher, PKCS57);
    out = cipher_block(&cipher, out, bklens, &size);
    CuAssertTrue(tc, 0 == memcmp(out, "12345678", 8));

    cipher_init(&cipher, AES, ECB, "cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC", strlen("cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC"), 256, 1);
    cipher_padding(&cipher, ISO10126);
    out = cipher_block(&cipher, "12345678", 8, &size);
    tohex(out, bklens, hex);
    cipher_init(&cipher, AES, ECB, "cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC", strlen("cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC"), 256, 0);
    cipher_padding(&cipher, ISO10126);
    out = cipher_block(&cipher, out, bklens, &size);
    CuAssertTrue(tc, 0 == memcmp(out, "12345678", 8) && 8 == ((char *)out)[15]);

    cipher_init(&cipher, AES, ECB, "cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC", strlen("cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC"), 256, 1);
    cipher_padding(&cipher, ANSIX923);
    out = cipher_block(&cipher, "12345678", 8, &size);
    tohex(out, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "33A3AEAFFEAA414C3BD4CEAB2D9890B0"));
    cipher_init(&cipher, AES, ECB, "cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC", strlen("cMEYqsmzxybOcUw4DPhgg4D2y6uJAuIC"), 256, 0);
    cipher_padding(&cipher, ANSIX923);
    out = cipher_block(&cipher, out, bklens, &size);
    CuAssertTrue(tc, 0 == memcmp(out, "12345678", 8));
}
static void test_cipher_cbc(CuTest* tc) {
    char hex[HEX_ENSIZE(16)];
    char out1[16];
    char out2[16];
    cipher_ctx cipher;
    size_t bklens;
    cipher_init(&cipher, DES, CBC, "bOcUw4DP", strlen("bOcUw4DP"), 0, 1);
    bklens = cipher_size(&cipher);
    cipher_iv(&cipher, "ybOc", 4);
    void *out = cipher_block(&cipher, "12345678", 8, NULL);
    memcpy(out1, out, bklens);
    tohex(out1, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "4EEF732BA8A71007"));
    out = cipher_block(&cipher, "abcdefgh", 8, NULL);
    memcpy(out2, out, bklens);
    tohex(out2, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "1C8305879464A797"));

    cipher_reset(&cipher);
    out = cipher_block(&cipher, "12345678", 8, NULL);
    memcpy(out1, out, bklens);
    tohex(out1, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "4EEF732BA8A71007"));
    out = cipher_block(&cipher, "abcdefgh", 8, NULL);
    memcpy(out2, out, bklens);
    tohex(out2, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "1C8305879464A797"));
    
    cipher_init(&cipher, DES, CBC, "bOcUw4DP", strlen("bOcUw4DP"), 0, 0);
    cipher_iv(&cipher, "ybOc", 4);
    out = cipher_block(&cipher, out1, 8, NULL);
    CuAssertTrue(tc, 0 == memcmp(out, "12345678", 8));
    out = cipher_block(&cipher, out2, 8, NULL);
    CuAssertTrue(tc, 0 == memcmp(out, "abcdefgh", 8));
    //AES
    cipher_init(&cipher, AES, CBC, "cMEYqsmzxybOcUw4DPhgg4D2y6u", strlen("cMEYqsmzxybOcUw4DPhgg4D2y6u"), 256, 1);
    bklens = cipher_size(&cipher);
    cipher_iv(&cipher, "bOcUw4DPDbRJ1aT&", strlen("bOcUw4DPDbRJ1aT&"));
    out = cipher_block(&cipher, "12345678abcdefgh", 16, NULL);
    memcpy(out1, out, bklens);
    tohex(out1, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "7255B1518E40815FFE99BF80013940A6"));
    out = cipher_block(&cipher, "abcdefgh12345678", 16, NULL);
    memcpy(out2, out, bklens);
    tohex(out2, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "DA4D5876ED487A0B30AB54D1B942D905"));

    cipher_reset(&cipher);
    out = cipher_block(&cipher, "12345678abcdefgh", 16, NULL);
    memcpy(out1, out, bklens);
    tohex(out1, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "7255B1518E40815FFE99BF80013940A6"));
    out = cipher_block(&cipher, "abcdefgh12345678", 16, NULL);
    memcpy(out2, out, bklens);
    tohex(out2, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "DA4D5876ED487A0B30AB54D1B942D905"));

    cipher_init(&cipher, AES, CBC, "cMEYqsmzxybOcUw4DPhgg4D2y6u", strlen("cMEYqsmzxybOcUw4DPhgg4D2y6u"), 256, 0);
    cipher_iv(&cipher, "bOcUw4DPDbRJ1aT&", strlen("bOcUw4DPDbRJ1aT&"));
    out = cipher_block(&cipher, out1, 16, NULL);
    CuAssertTrue(tc, 0 == memcmp(out, "12345678abcdefgh", 16));
    out = cipher_block(&cipher, out2, 16, NULL);
    CuAssertTrue(tc, 0 == memcmp(out, "abcdefgh12345678", 16));
}
static void test_cipher_cfb(CuTest* tc) {
    char hex[HEX_ENSIZE(16)];
    char out1[16];
    char out2[16];
    char out3[16];
    cipher_ctx cipher;
    size_t bklens;
    size_t size;
    cipher_init(&cipher, DES, CFB, "bOcUw4DP", strlen("bOcUw4DP"), 0, 1);
    bklens = cipher_size(&cipher);
    cipher_iv(&cipher, "ybOc", 4);
    void *out = cipher_block(&cipher, "12345678", 8, NULL);
    memcpy(out1, out, bklens);
    tohex(out1, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "CF55CE0D5E865FEA"));
    out = cipher_block(&cipher, "abcdefgh", 8, NULL);
    memcpy(out2, out, bklens);
    tohex(out2, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "47EFF5E1D24DDE7A"));
    out = cipher_block(&cipher, "xyzq", 4, &size);
    memcpy(out3, out, size);
    tohex(out3, size, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "BC298FFF"));

    cipher_init(&cipher, DES, CFB, "bOcUw4DP", strlen("bOcUw4DP"), 0, 0);
    cipher_iv(&cipher, "ybOc", 4);
    out = cipher_block(&cipher, out1, 8, NULL);
    CuAssertTrue(tc, 0 == memcmp(out, "12345678", 8));
    out = cipher_block(&cipher, out2, 8, NULL);
    CuAssertTrue(tc, 0 == memcmp(out, "abcdefgh", 8));
    out = cipher_block(&cipher, out3, size, &size);
    CuAssertTrue(tc, 0 == memcmp(out, "xyzq", size));
    //AES
    cipher_init(&cipher, AES, CFB, "cMEYqsmzxybOcUw4DPhgg4D2y6u", strlen("cMEYqsmzxybOcUw4DPhgg4D2y6u"), 256, 1);
    bklens = cipher_size(&cipher);
    cipher_iv(&cipher, "bOcUw4DPDbRJ1aT&", strlen("bOcUw4DPDbRJ1aT&"));
    out = cipher_block(&cipher, "12345678abcdefgh", 16, NULL);
    memcpy(out1, out, bklens);
    tohex(out1, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "68830B207697A753E05578EAED7B222C"));
    out = cipher_block(&cipher, "abcdefgh12345678", 16, NULL);
    memcpy(out2, out, bklens);
    tohex(out2, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "D09494F1056B7B54CFE8ECA7621C0FA8"));
    out = cipher_block(&cipher, "xyzq", 4, &size);
    memcpy(out3, out, size);
    tohex(out3, size, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "0C77DB35"));

    cipher_init(&cipher, AES, CFB, "cMEYqsmzxybOcUw4DPhgg4D2y6u", strlen("cMEYqsmzxybOcUw4DPhgg4D2y6u"), 256, 0);
    cipher_iv(&cipher, "bOcUw4DPDbRJ1aT&", strlen("bOcUw4DPDbRJ1aT&"));
    out = cipher_block(&cipher, out1, 16, NULL);
    CuAssertTrue(tc, 0 == memcmp(out, "12345678abcdefgh", 16));
    out = cipher_block(&cipher, out2, 16, NULL);
    CuAssertTrue(tc, 0 == memcmp(out, "abcdefgh12345678", 16));
    out = cipher_block(&cipher, out3, size, &size);
    CuAssertTrue(tc, 0 == memcmp(out, "xyzq", size));
}
static void test_cipher_ofb(CuTest* tc) {
    char hex[HEX_ENSIZE(16)];
    char out1[16];
    char out2[16];
    char out3[16];
    cipher_ctx cipher;
    size_t bklens;
    size_t size;
    cipher_init(&cipher, DES, OFB, "bOcUw4DP", strlen("bOcUw4DP"), 0, 1);
    bklens = cipher_size(&cipher);
    cipher_iv(&cipher, "ybOcybOc", 8);
    void *out = cipher_block(&cipher, "12345678", 8, NULL);
    memcpy(out1, out, bklens);
    tohex(out1, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "FB1F70EBC1F82512"));
    out = cipher_block(&cipher, "abcdefgh", 8, NULL);
    memcpy(out2, out, bklens);
    tohex(out2, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "AC928BBF2C0EDF9B"));
    out = cipher_block(&cipher, "xyzq", 4, &size);
    memcpy(out3, out, size);
    tohex(out3, size, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "BFB208B8"));

    cipher_init(&cipher, DES, OFB, "bOcUw4DP", strlen("bOcUw4DP"), 0, 0);
    cipher_iv(&cipher, "ybOcybOc", 8);
    out = cipher_block(&cipher, out1, 8, NULL);
    CuAssertTrue(tc, 0 == memcmp(out, "12345678", 8));
    out = cipher_block(&cipher, out2, 8, NULL);
    CuAssertTrue(tc, 0 == memcmp(out, "abcdefgh", 8));
    out = cipher_block(&cipher, out3, size, &size);
    CuAssertTrue(tc, 0 == memcmp(out, "xyzq", size));
    //AES
    cipher_init(&cipher, AES, OFB, "cMEYqsmzxybOcUw4DPhgg4D2y6u", strlen("cMEYqsmzxybOcUw4DPhgg4D2y6u"), 256, 1);
    bklens = cipher_size(&cipher);
    cipher_iv(&cipher, "bOcUw4DPDbRJ1aT&", strlen("bOcUw4DPDbRJ1aT&"));
    out = cipher_block(&cipher, "12345678abcdefgh", 16, NULL);
    memcpy(out1, out, bklens);
    tohex(out1, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "68830B207697A753E05578EAED7B222C"));
    out = cipher_block(&cipher, "abcdefgh12345678", 16, NULL);
    memcpy(out2, out, bklens);
    tohex(out2, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "E98A7CE553F6C5D8E6E1237F2E52722D"));
    out = cipher_block(&cipher, "xyzq", 4, &size);
    memcpy(out3, out, size);
    tohex(out3, size, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "38E8C199"));

    cipher_init(&cipher, AES, OFB, "cMEYqsmzxybOcUw4DPhgg4D2y6u", strlen("cMEYqsmzxybOcUw4DPhgg4D2y6u"), 256, 0);
    cipher_iv(&cipher, "bOcUw4DPDbRJ1aT&", strlen("bOcUw4DPDbRJ1aT&"));
    out = cipher_block(&cipher, out1, 16, NULL);
    CuAssertTrue(tc, 0 == memcmp(out, "12345678abcdefgh", 16));
    out = cipher_block(&cipher, out2, 16, NULL);
    CuAssertTrue(tc, 0 == memcmp(out, "abcdefgh12345678", 16));
    out = cipher_block(&cipher, out3, size, &size);
    CuAssertTrue(tc, 0 == memcmp(out, "xyzq", size));
}
static void test_cipher_ctr(CuTest* tc) {
    char hex[HEX_ENSIZE(16)];
    char out1[16];
    char out2[16];
    cipher_ctx cipher;
    size_t bklens;
    size_t size;
    cipher_init(&cipher, DES, CTR, "bOcUw4DP", strlen("bOcUw4DP"), 0, 1);
    bklens = cipher_size(&cipher);
    cipher_iv(&cipher, "ybOc", 4);
    void *out = cipher_block(&cipher, "12345678", 8, NULL);
    memcpy(out1, out, bklens);
    tohex(out1, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "CF55CE0D5E865FEA"));
    out = cipher_block(&cipher, "xyzq", 4, &size);
    memcpy(out2, out, size);
    tohex(out2, size, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "E59EAD64"));

    cipher_init(&cipher, DES, CTR, "bOcUw4DP", strlen("bOcUw4DP"), 0, 0);
    cipher_iv(&cipher, "ybOc", 4);
    out = cipher_block(&cipher, out1, 8, NULL);
    CuAssertTrue(tc, 0 == memcmp(out, "12345678", 8));
    out = cipher_block(&cipher, out2, 4, NULL);
    CuAssertTrue(tc, 0 == memcmp(out, "xyzq", 4));
    //AES
    cipher_init(&cipher, AES, CTR, "cMEYqsmzxybOcUw4DPhgg4D2y6u", strlen("cMEYqsmzxybOcUw4DPhgg4D2y6u"), 256, 1);
    bklens = cipher_size(&cipher);
    cipher_iv(&cipher, "bOcUw4DP", strlen("bOcUw4DP"));
    out = cipher_block(&cipher, "12345678abcdefgh", 16, NULL);
    memcpy(out1, out, bklens);
    tohex(out1, bklens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "38B5E0BB4C654722315C918855322DFF"));
    out = cipher_block(&cipher, "xyzq", 4, &size);
    memcpy(out2, out, size);
    tohex(out2, size, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "63CA3B21"));

    cipher_init(&cipher, AES, CTR, "cMEYqsmzxybOcUw4DPhgg4D2y6u", strlen("cMEYqsmzxybOcUw4DPhgg4D2y6u"), 256, 0);
    cipher_iv(&cipher, "bOcUw4DP", strlen("bOcUw4DP"));
    out = cipher_block(&cipher, out1, 16, NULL);
    CuAssertTrue(tc, 0 == memcmp(out, "12345678abcdefgh", 16));
    out = cipher_block(&cipher, out2, 4, NULL);
    CuAssertTrue(tc, 0 == memcmp(out, "xyzq", 4));
}
static void test_cipher(CuTest* tc) {
    char enbuf[512];
    char debuf[512];
    char hex[512];
    cipher_ctx en;
    cipher_ctx de;
    cipher_init(&en, DES, CTR, "cMEYqsmu", strlen("cMEYqsmu"), 0, 1);
    cipher_padding(&en, ZeroPadding);
    cipher_iv(&en, "bOcU", strlen("bOcU"));
    cipher_init(&de, DES, CTR, "cMEYqsmu", strlen("cMEYqsmu"), 0, 0);
    cipher_padding(&de, ZeroPadding);
    cipher_iv(&de, "bOcU", strlen("bOcU"));

    size_t lens = cipher_dofinal(&en, "12345", 5, enbuf);
    tohex(enbuf, lens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "EBDB5957790A9E24"));
    ZERO(debuf, sizeof(debuf));
    lens = cipher_dofinal(&de, enbuf, lens, debuf);
    CuAssertTrue(tc, 0 == strcmp(debuf, "12345") && 8 == lens);

    lens = cipher_dofinal(&en, "12345678", 8, enbuf);
    tohex(enbuf, lens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "EBDB5957793CA91C"));
    ZERO(debuf, sizeof(debuf));
    lens = cipher_dofinal(&de, enbuf, lens, debuf);
    CuAssertTrue(tc, 0 == strcmp(debuf, "12345678") && 8 == lens);

    lens = cipher_dofinal(&en, "1234567890", 10, enbuf);
    tohex(enbuf, lens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "EBDB5957793CA91CED4FD8736AB3EB57"));
    lens = cipher_dofinal(&de, enbuf, lens, debuf);
    CuAssertTrue(tc, 0 == strcmp(debuf, "1234567890") && 16 == lens);
    //
    cipher_padding(&en, PKCS57);
    cipher_padding(&de, PKCS57);
    lens = cipher_dofinal(&en, "12345", 5, enbuf);
    tohex(enbuf, lens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "EBDB595779099D27"));
    lens = cipher_dofinal(&de, enbuf, lens, debuf);
    debuf[lens] = '\0';
    CuAssertTrue(tc, 0 == strcmp(debuf, "12345") && 5 == lens);

    lens = cipher_dofinal(&en, "12345678", 8, enbuf);
    tohex(enbuf, lens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "EBDB5957793CA91CDC77D07B62BBE35F"));
    lens = cipher_dofinal(&de, enbuf, lens, debuf);
    debuf[lens] = '\0';
    CuAssertTrue(tc, 0 == strcmp(debuf, "12345678") && 8 == lens);

    lens = cipher_dofinal(&en, "1234567890", 10, enbuf);
    tohex(enbuf, lens, hex);
    CuAssertTrue(tc, 0 == strcmp(hex, "EBDB5957793CA91CED4FDE756CB5ED51"));
    lens = cipher_dofinal(&de, enbuf, lens, debuf);
    debuf[lens] = '\0';
    CuAssertTrue(tc, 0 == strcmp(debuf, "1234567890") && 10 == lens);
}
static void test_timer(CuTest* tc) {
    timer_ctx timer;
    timer_init(&timer);
    timer_start(&timer);
    MSLEEP(100);
    PRINT("timer_elapsed_ms: %"PRIu64"", timer_elapsed_ms(&timer));
}
static void test_netutils(CuTest* tc) {
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
static void test_buffer(CuTest* tc) {
    buffer_ctx buf;
    buffer_init(&buf); 
    char c;
    const char *str1 = "this is test.";
    const char *str2 = "who am i?";
    CuAssertTrue(tc, ERR_OK == buffer_append(&buf, (void *)str1, strlen(str1)));
    CuAssertTrue(tc, ERR_OK == buffer_appendv(&buf, "%s", str2));
    char tmp[1024] = { 0 };
    for (size_t i = 0; i < buffer_size(&buf); i++) {
        c = buffer_at(&buf, i);
        tmp[i] = c;
    }
    CuAssertTrue(tc, 0 == strcmp("this is test.who am i?", tmp));
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
static void test_log(CuTest* tc) {
    LOG_DEBUG("%s", "LOG_DEBUG");
    LOG_INFO("%s", "LOG_INFO");
    LOG_WARN("%s", "LOG_WARN");
    LOG_ERROR("%s", "LOG_ERROR");
    LOG_FATAL("%s", "LOG_FATAL");
}
static void test_http(CuTest* tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    int32_t status = 0;
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.pktype = PACK_HTTP;
    const char *http1 = "POST /users HTTP/1.1\r\n  Host:   api.github.com\r\nContent-Length: 5\r\na: \r\n\r\n1";
    buffer_append(&buf, (void *)http1, strlen(http1));
    void *rtnbuf = http_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL == rtnbuf);
    const char *http2 = "2345";
    buffer_append(&buf, (void *)http2, strlen(http2));
    rtnbuf = http_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL != rtnbuf);
    protos_pkfree(PACK_HTTP, 0, rtnbuf);

    const char *http3 = "POST /users HTTP/1.1\r\nHost: api.github.com\r\nContent-Length: 5\r\n\r\n12345";
    buffer_append(&buf, (void *)http3, strlen(http3));
    rtnbuf = http_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL != rtnbuf);
    protos_pkfree(PACK_HTTP, 0, rtnbuf);

    const char *http4 = "POST /users HTTP/1.1\r\nHost: api.github.com\r\n\r\n";
    buffer_append(&buf, (void *)http4, strlen(http4));
    rtnbuf = http_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL != rtnbuf);
    protos_pkfree(PACK_HTTP, 0, rtnbuf);

    const char *http5 = "POST /users HTTP/1.1\r\nHost: api.github.com\r\nTransfer-Encoding: chunked\r\n\r\n7\r\nMozilla\r\nb\r\nDeveloper N\r\n0\r\n\r\n";
    buffer_append(&buf, (void *)http5, strlen(http5));
    rtnbuf = http_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL != rtnbuf && BIT_CHECK(status, PROTO_SLICE_START));
    protos_pkfree(PACK_HTTP, 0, rtnbuf);
    rtnbuf = http_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL != rtnbuf && BIT_CHECK(status, PROTO_SLICE));
    protos_pkfree(PACK_HTTP, 0, rtnbuf);
    rtnbuf = http_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL != rtnbuf && BIT_CHECK(status, PROTO_SLICE));
    protos_pkfree(PACK_HTTP, 0, rtnbuf);
    rtnbuf = http_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL != rtnbuf && BIT_CHECK(status, PROTO_SLICE_END));
    protos_pkfree(PACK_HTTP, 0, rtnbuf);
    protos_udfree(&ud);

    buffer_free(&buf);
}
static void test_url(CuTest* tc) {
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
static void test_redis_pack(CuTest* tc) {
    size_t size;
    size_t blens = strlen("binary");
    char *cmd = redis_pack(&size, "  Test %d   %.2f %hhd  %hd %lld  %ld %s %b  %%  end  ",
        1, 2.015, 3, 4, (long long)5, (long)6, "text", "binary", blens);
    CuAssertTrue(tc, 0 == strcmp("*11\r\n$4\r\nTest\r\n$1\r\n1\r\n$4\r\n2.02\r\n$1\r\n3\r\n$1\r\n4\r\n$1\r\n5\r\n$1\r\n6\r\n$4\r\ntext\r\n$6\r\nbinary\r\n$1\r\n%\r\n$3\r\nend\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, "  ", 65, &cmd, 123);
    CuAssertTrue(tc, 0 == strcmp("*0\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, " % d%s ", 10, "abc");
    CuAssertTrue(tc, 0 == strcmp("*1\r\n$6\r\n 10abc\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, " % d %s ", -10, "abc");
    CuAssertTrue(tc, 0 == strcmp("*2\r\n$3\r\n-10\r\n$3\r\nabc\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, " SET  KEY1  TEST1  ");
    CuAssertTrue(tc, 0 == strcmp("*3\r\n$3\r\nSET\r\n$4\r\nKEY1\r\n$5\r\nTEST1\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, "%s%b%d%.2f end", "text", "binary", blens, 123, 2.015);
    CuAssertTrue(tc, 0 == strcmp("*2\r\n$17\r\ntextbinary1232.02\r\n$3\r\nend\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, " %s%b%d%.2f end", "text", "binary", blens, 123, 2.015);
    CuAssertTrue(tc, 0 == strcmp("*2\r\n$17\r\ntextbinary1232.02\r\n$3\r\nend\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, "%s%b%d%.2f", "text", "binary", blens, 123, 2.015);
    CuAssertTrue(tc, 0 == strcmp("*1\r\n$17\r\ntextbinary1232.02\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, "%s%b%d%.2f ", "text", "binary", blens, 123, 2.015);
    CuAssertTrue(tc, 0 == strcmp("*1\r\n$17\r\ntextbinary1232.02\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, "Set %s%b%d%.2f %lld%% ", "text", "binary", blens, 123, 2.015, (long long)456);
    CuAssertTrue(tc, 0 == strcmp("*3\r\n$3\r\nSet\r\n$17\r\ntextbinary1232.02\r\n$4\r\n456%\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, "Set %s%b%d%.2f %lld%%", "text", "binary", blens, 123, 2.015, (long long)456);
    CuAssertTrue(tc, 0 == strcmp("*3\r\n$3\r\nSet\r\n$17\r\ntextbinary1232.02\r\n$4\r\n456%\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, "%");
    CuAssertTrue(tc, 0 == strcmp("*0\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, "%#0-+ .124");
    CuAssertTrue(tc, 0 == strcmp("*1\r\n$9\r\n#0-+ .124\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, "%#0-+ .124  ");
    CuAssertTrue(tc, 0 == strcmp("*1\r\n$9\r\n#0-+ .124\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, "%hh");
    CuAssertTrue(tc, 0 == strcmp("*1\r\n$2\r\nhh\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, "%hhq %.2f", 123.456);
    CuAssertTrue(tc, 0 == strcmp("*2\r\n$3\r\nhhq\r\n$6\r\n123.46\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, "%h %.2f ", 123.456);
    CuAssertTrue(tc, 0 == strcmp("*2\r\n$1\r\nh\r\n$6\r\n123.46\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, "%llq %.2f", 123.456);
    CuAssertTrue(tc, 0 == strcmp("*2\r\n$3\r\nllq\r\n$6\r\n123.46\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, "%l %.2f ", 123.456);
    CuAssertTrue(tc, 0 == strcmp("*2\r\n$1\r\nl\r\n$6\r\n123.46\r\n", cmd));
    FREE(cmd);
    cmd = redis_pack(&size, "%%%l %.2f ", 123.456);
    CuAssertTrue(tc, 0 == strcmp("*2\r\n$2\r\n%l\r\n$6\r\n123.46\r\n", cmd));
    FREE(cmd);
}
static void test_redis_unpack(CuTest* tc) {
    int32_t status = 0;
    ud_cxt ud;
    ZERO(&ud, sizeof(ud_cxt));
    buffer_ctx buf;
    buffer_init(&buf);
    //simple string
    buffer_appendv(&buf, "%s", "+\r\n");
    redis_pack_ctx *pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_STRING == pk->proto && 0 == pk->len);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "+OK\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_STRING == pk->proto && 0 == strcmp("OK", pk->data));
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "-Error message\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_ERROR == pk->proto && 0 == strcmp("Error message", pk->data));
    _redis_pkfree(pk);
    //INTEGER big number
    buffer_appendv(&buf, "%s", "(\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL == pk && BIT_CHECK(status, PROTO_ERROR));
    status = 0;
    buffer_drain(&buf, buffer_size(&buf));
    buffer_appendv(&buf, "%s", "(123a\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL == pk && BIT_CHECK(status, PROTO_ERROR));
    status = 0;
    buffer_drain(&buf, buffer_size(&buf));

    buffer_appendv(&buf, "%s", ":-123\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_INTEGER == pk->proto && -123 == pk->ival);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "(12345678\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_BIGNUM == pk->proto && 12345678 == pk->ival);
    _redis_pkfree(pk);
    //NULL
    buffer_appendv(&buf, "%s", "_a\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL == pk && BIT_CHECK(status, PROTO_ERROR));
    status = 0;
    buffer_drain(&buf, buffer_size(&buf));

    buffer_appendv(&buf, "%s", "_\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_NIL == pk->proto);
    _redis_pkfree(pk);
    //BOOL
    buffer_appendv(&buf, "%s", "#\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL == pk && BIT_CHECK(status, PROTO_ERROR));
    status = 0;
    buffer_drain(&buf, buffer_size(&buf));
    buffer_appendv(&buf, "%s", "#A\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL == pk && BIT_CHECK(status, PROTO_ERROR));
    status = 0;
    buffer_drain(&buf, buffer_size(&buf));

    buffer_appendv(&buf, "%s", "#T\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_BOOL == pk->proto && 1 == pk->ival);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "#f\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_BOOL == pk->proto && 0 == pk->ival);
    _redis_pkfree(pk);
    //DOUBLE
    buffer_appendv(&buf, "%s", ",\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL == pk);
    status = 0;
    buffer_drain(&buf, buffer_size(&buf));
    buffer_appendv(&buf, "%s", ",12.354a\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL == pk);
    status = 0;
    buffer_drain(&buf, buffer_size(&buf));

    buffer_appendv(&buf, "%s", ",inf\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_DOUBLE == pk->proto);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", ",-inf\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_DOUBLE == pk->proto);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", ",NAN\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_DOUBLE == pk->proto);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", ",-nan\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_DOUBLE == pk->proto);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", ",12.345\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_DOUBLE == pk->proto);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", ",-1.23400E-03\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_DOUBLE == pk->proto);
    _redis_pkfree(pk);
    
    //BULK
    buffer_appendv(&buf, "%s", "!\r\n\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL == pk && BIT_CHECK(status, PROTO_ERROR));
    status = 0;
    buffer_drain(&buf, buffer_size(&buf));
    buffer_appendv(&buf, "%s", "!10A\r\n\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL == pk && BIT_CHECK(status, PROTO_ERROR));
    status = 0;
    buffer_drain(&buf, buffer_size(&buf));
    buffer_appendv(&buf, "%s", "!5\r\nError\rA");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL == pk && BIT_CHECK(status, PROTO_ERROR));
    status = 0;
    buffer_drain(&buf, buffer_size(&buf));
    buffer_appendv(&buf, "%s", "=9\r\ntxtAhello\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, pk == NULL && BIT_CHECK(status, PROTO_ERROR));
    status = 0;
    buffer_drain(&buf, buffer_size(&buf));
    buffer_appendv(&buf, "%s", "=3\r\ntxt\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, pk == NULL && BIT_CHECK(status, PROTO_ERROR));
    status = 0;
    buffer_drain(&buf, buffer_size(&buf));

    buffer_appendv(&buf, "%s", "$-1\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_BSTRING == pk->proto && -1 == pk->len);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "$0\r\n\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_BSTRING == pk->proto && 0 == pk->len);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "$5\r\nhello\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_BSTRING == pk->proto && 0 == strcmp("hello", pk->data));
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "!5\r\nError\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_BERROR == pk->proto && 0 == strcmp("Error", pk->data));
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "!0\r\n\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_BERROR == pk->proto && 0 == pk->len);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "!5\r\nError\r");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL == pk && !BIT_CHECK(status, PROTO_ERROR));
    buffer_appendv(&buf, "%s", "\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_BERROR == pk->proto && 0 == strcmp("Error", pk->data));
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "=9\r\ntxt:hello\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_VERB == pk->proto && 0 == strcmp("txt", pk->venc) && 0 == strcmp("hello", pk->data));
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "=4\r\ntxt:\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && RESP_VERB == pk->proto && 0 == strcmp("txt", pk->venc) && 0 == pk->len);
    _redis_pkfree(pk);

    //Aggregate
    buffer_appendv(&buf, "%s", "*\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL == pk && BIT_CHECK(status, PROTO_ERROR));
    status = 0;
    buffer_drain(&buf, buffer_size(&buf));
    buffer_appendv(&buf, "%s", "*10A\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL == pk && BIT_CHECK(status, PROTO_ERROR));
    status = 0;
    buffer_drain(&buf, buffer_size(&buf));

    buffer_appendv(&buf, "%s", "*0\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == pk->nelem && 0 == buffer_size(&buf));
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "*-1\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, -1 == pk->nelem && 0 == buffer_size(&buf));
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "|0\r\n+test\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == pk->nelem && 0 == buffer_size(&buf) && 0 == strcmp("test", pk->next->data));
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "|-1\r\n+test\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, -1 == pk->nelem && 0 == buffer_size(&buf) && 0 == strcmp("test", pk->next->data));
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "*3\r\n$3\r\nSET\r\n$4\r\nK");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, NULL == pk && !BIT_CHECK(status, PROTO_ERROR));
    buffer_appendv(&buf, "%s", "EY1\r\n$5\r\nTEST1\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && 0 == strcmp("SET", pk->next->data) && 0 == strcmp("KEY1", pk->next->next->data) && 0 == strcmp("TEST1", pk->next->next->next->data));
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "*2\r\n*3\r\n:1\r\n$5\r\nhello\r\n:2\r\n#f\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && NULL != pk);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "%2\r\n+first\r\n:1\r\n+second\r\n:2\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && 0 == strcmp("first", pk->next->data) && 2 ==  pk->next->next->next->next->ival);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "~5\r\n+orange\r\n+apple\r\n#t\r\n:100\r\n:999\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && NULL != pk);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "|1\r\n+key-popularity\r\n%2\r\n$1\r\na\r\n,0.1923\r\n$1\r\nb\r\n,0.0012\r\n*2\r\n:2039123\r\n:9543892\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && NULL != pk);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "*3\r\n:1\r\n:2\r\n|1\r\n+ttl\r\n:3600\r\n:3\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && NULL != pk);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", ">3\r\n+message\r\n+somechannel\r\n+this is the message\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && NULL != pk);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "$9\r\nGet-Reply\r\n>3\r\n+message\r\n+somechannel\r\n+this is the message\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 != buffer_size(&buf) && NULL != pk);
    _redis_pkfree(pk);
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && NULL != pk);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", ">3\r\n+message\r\n+somechannel\r\n+this is the message\r\n$9\r\nGet-Reply\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 != buffer_size(&buf) && NULL != pk);
    _redis_pkfree(pk);
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && NULL != pk);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "*4\r\n%2\r\n+first\r\n:1\r\n+second\r\n:2\r\n%2\r\n+first2\r\n:3\r\n+second2\r\n:4\r\n,12.345\r\n$5\r\nhello\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && NULL != pk);
    _redis_pkfree(pk);
    buffer_appendv(&buf, "%s", "*6\r\n%2\r\n%1\r\n+first1\r\n:1\r\n%1\r\n+first2\r\n:2\r\n+second1\r\n:2\r\n%2\r\n+first3\r\n:3\r\n|1\r\n+ttl\r\n:3600\r\n+second2\r\n:4\r\n,12.345\r\n$5\r\nhello\r\n%0\r\n*1\r\n+array\r\n");
    pk = redis_unpack(&buf, &ud, &status);
    CuAssertTrue(tc, 0 == buffer_size(&buf) && NULL != pk);
    _redis_pkfree(pk);

    buffer_free(&buf);
    _redis_udfree(&ud);
}
void test_utils(CuSuite* suite) {
    SUITE_ADD_TEST(suite, test_array);
    SUITE_ADD_TEST(suite, test_queue);
    SUITE_ADD_TEST(suite, test_heap);
    SUITE_ADD_TEST(suite, test_system);
    SUITE_ADD_TEST(suite, test_crypt_other);
    SUITE_ADD_TEST(suite, test_digest);
    SUITE_ADD_TEST(suite, test_cipher_ecb);
    SUITE_ADD_TEST(suite, test_cipher_cbc);
    SUITE_ADD_TEST(suite, test_cipher_cfb);
    SUITE_ADD_TEST(suite, test_cipher_ofb);
    SUITE_ADD_TEST(suite, test_cipher_ctr);
    SUITE_ADD_TEST(suite, test_cipher);
    SUITE_ADD_TEST(suite, test_timer);
    SUITE_ADD_TEST(suite, test_netutils);
    SUITE_ADD_TEST(suite, test_buffer);
    SUITE_ADD_TEST(suite, test_log);
    SUITE_ADD_TEST(suite, test_http);
    SUITE_ADD_TEST(suite, test_url);
    SUITE_ADD_TEST(suite, test_redis_pack);
    SUITE_ADD_TEST(suite, test_redis_unpack);
}
