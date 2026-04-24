#include "test_crypt.h"
#include "lib.h"

/* =======================================================================
 * base64 编解码
 * ======================================================================= */
static void test_base64(CuTest *tc) {
    char enc[256];
    char dec[256];
    size_t elen, dlen;

    /* 已知向量：RFC 4648 标准样例 */
    const char *cases[][2] = {
        { "",       ""         },
        { "f",      "Zg=="     },
        { "fo",     "Zm8="     },
        { "foo",    "Zm9v"     },
        { "hello",  "aGVsbG8=" },
        { "Man",    "TWFu"     },
    };
    int n = (int)(sizeof(cases) / sizeof(cases[0]));

    for (int i = 0; i < n; i++) {
        const char *plain  = cases[i][0];
        const char *expect = cases[i][1];
        size_t plen = strlen(plain);

        /* 编码 */
        elen = bs64_encode(plain, plen, enc);
        enc[elen] = '\0';
        CuAssertStrEquals(tc, expect, enc);

        /* 解码还原 */
        dlen = bs64_decode(enc, elen, dec);
        dec[dlen] = '\0';
        CuAssertTrue(tc, plen == dlen);
        CuAssertTrue(tc, 0 == memcmp(plain, dec, plen));
    }

    /* 二进制数据往返验证（含 \0 字节）*/
    char bin[16];
    for (int i = 0; i < 16; i++) bin[i] = (char)i;
    elen = bs64_encode(bin, 16, enc);
    dlen = bs64_decode(enc, elen, dec);
    CuAssertTrue(tc, 16 == dlen);
    CuAssertTrue(tc, 0 == memcmp(bin, dec, 16));
}

/* =======================================================================
 * CRC-16 / CRC-32
 * ======================================================================= */
static void test_crc(CuTest *tc) {
    const char *data = "123456789";
    size_t len = strlen(data);

    /* CRC-16 IBM 标准值 */
    uint16_t c16 = crc16(data, len);
    CuAssertTrue(tc, 0xBB3D == c16);

    /* 空数据不崩溃 */
    crc16("", 0);
    crc32("", 0);

    /* CRC-32 标准值（IEEE 802.3）*/
    uint32_t c32 = crc32(data, len);
    CuAssertTrue(tc, 0xCBF43926 == c32);

    /* 相同数据结果一致 */
    CuAssertTrue(tc, c16 == crc16(data, len));
    CuAssertTrue(tc, c32 == crc32(data, len));

    /* 不同数据结果不同 */
    CuAssertTrue(tc, c32 != crc32("12345678", len - 1));
}

/* =======================================================================
 * digest（MD5 / SHA1 / SHA256 / SHA512）
 * ======================================================================= */

/* 将 hash 字节转换为十六进制字符串 */
static void _to_hex(const char *hash, size_t hlen, char *out) {
    static const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < hlen; i++) {
        out[i * 2]     = hex[((unsigned char)hash[i]) >> 4];
        out[i * 2 + 1] = hex[((unsigned char)hash[i]) & 0x0f];
    }
    out[hlen * 2] = '\0';
}

static void test_digest(CuTest *tc) {
    char hash[DG_BLOCK_SIZE];
    char hex[DG_BLOCK_SIZE * 2 + 1];
    digest_ctx dg;
    size_t hlen;

    /* MD5("") */
    digest_init(&dg, DG_MD5);
    digest_update(&dg, "", 0);
    hlen = digest_final(&dg, hash);
    _to_hex(hash, hlen, hex);
    CuAssertStrEquals(tc, "d41d8cd98f00b204e9800998ecf8427e", hex);

    /* MD5("abc") */
    digest_init(&dg, DG_MD5);
    digest_update(&dg, "abc", 3);
    hlen = digest_final(&dg, hash);
    _to_hex(hash, hlen, hex);
    CuAssertStrEquals(tc, "900150983cd24fb0d6963f7d28e17f72", hex);

    /* SHA1("abc") */
    digest_init(&dg, DG_SHA1);
    digest_update(&dg, "abc", 3);
    hlen = digest_final(&dg, hash);
    _to_hex(hash, hlen, hex);
    CuAssertStrEquals(tc, "a9993e364706816aba3e25717850c26c9cd0d89d", hex);

    /* SHA256("") */
    digest_init(&dg, DG_SHA256);
    digest_update(&dg, "", 0);
    hlen = digest_final(&dg, hash);
    _to_hex(hash, hlen, hex);
    CuAssertStrEquals(tc, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", hex);

    /* SHA256("abc") */
    digest_init(&dg, DG_SHA256);
    digest_update(&dg, "abc", 3);
    hlen = digest_final(&dg, hash);
    _to_hex(hash, hlen, hex);
    CuAssertStrEquals(tc, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", hex);

    /* SHA512("abc") */
    digest_init(&dg, DG_SHA512);
    digest_update(&dg, "abc", 3);
    hlen = digest_final(&dg, hash);
    _to_hex(hash, hlen, hex);
    CuAssertStrEquals(tc,
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
        hex);

    /* reset 后可复用 */
    digest_reset(&dg);
    digest_update(&dg, "abc", 3);
    size_t hlen2 = digest_final(&dg, hash);
    CuAssertTrue(tc, hlen == hlen2);

    /* 分段 update 与整体 update 结果相同 */
    char h1[DG_BLOCK_SIZE], h2[DG_BLOCK_SIZE];
    digest_init(&dg, DG_SHA256);
    digest_update(&dg, "abcdef", 6);
    digest_final(&dg, h1);

    digest_init(&dg, DG_SHA256);
    digest_update(&dg, "abc", 3);
    digest_update(&dg, "def", 3);
    digest_final(&dg, h2);
    CuAssertTrue(tc, 0 == memcmp(h1, h2, digest_size(&dg)));
}

/* =======================================================================
 * HMAC
 * ======================================================================= */
static void test_hmac(CuTest *tc) {
    char hash[DG_BLOCK_SIZE];
    char hex[DG_BLOCK_SIZE * 2 + 1];
    hmac_ctx hm;
    size_t hlen;

    /* HMAC-SHA256("key", "The quick brown fox jumps over the lazy dog") */
    hmac_init(&hm, DG_SHA256, "key", 3);
    hmac_update(&hm, "The quick brown fox jumps over the lazy dog", 43);
    hlen = hmac_final(&hm, hash);
    _to_hex(hash, hlen, hex);
    CuAssertStrEquals(tc,
        "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8",
        hex);

    /* reset 后计算相同结果 */
    hmac_reset(&hm);
    hmac_update(&hm, "The quick brown fox jumps over the lazy dog", 43);
    char hash2[DG_BLOCK_SIZE];
    hmac_final(&hm, hash2);
    CuAssertTrue(tc, 0 == memcmp(hash, hash2, hlen));

    /* 不同 key → 不同结果 */
    hmac_init(&hm, DG_SHA256, "other", 5);
    hmac_update(&hm, "The quick brown fox jumps over the lazy dog", 43);
    hmac_final(&hm, hash2);
    CuAssertTrue(tc, 0 != memcmp(hash, hash2, hlen));
}

/* =======================================================================
 * URL 编解码
 * ======================================================================= */
static void test_urlraw(CuTest *tc) {
    char enc[512];
    char dec[512];
    size_t dlen;

    /* 编码：空格 → %20，特殊字符均被转义 */
    const char *plain = "hello world! foo=bar&a=1";
    url_encode(plain, strlen(plain), enc);
    CuAssertTrue(tc, NULL == strchr(enc, ' '));
    CuAssertTrue(tc, NULL == strchr(enc, '!'));

    /* 解码还原 */
    strncpy(dec, enc, sizeof(dec) - 1);
    dlen = url_decode(dec, strlen(dec));
    CuAssertTrue(tc, dlen == strlen(plain));
    CuAssertTrue(tc, 0 == memcmp(dec, plain, dlen));

    /* 纯 ASCII 字母数字不被转义，往返后相同 */
    const char *alpha = "abcABC012";
    url_encode(alpha, strlen(alpha), enc);
    CuAssertStrEquals(tc, alpha, enc);

    /* 空字符串 */
    url_encode("", 0, enc);
    CuAssertTrue(tc, '\0' == enc[0]);
}

/* =======================================================================
 * XOR 编解码
 * ======================================================================= */
static void test_xor(CuTest *tc) {
    const char key[4] = { 0x12, 0x34, 0x56, 0x78 };

    /* 单轮往返：encode 后 decode 还原 */
    char data[] = "Hello, World! This is a test for xor cipher.";
    size_t lens = sizeof(data) - 1;
    char origin[64];
    memcpy(origin, data, lens);

    xor_encode(key, 1, data, lens);
    /* 编码后数据与原文不同 */
    CuAssertTrue(tc, 0 != memcmp(origin, data, lens));

    xor_decode(key, 1, data, lens);
    /* 解码后还原 */
    CuAssertTrue(tc, 0 == memcmp(origin, data, lens));

    /* 多轮（round=3）往返 */
    char data2[] = "Hello, World! This is a test for xor cipher.";
    memcpy(origin, data2, lens);
    xor_encode(key, 3, data2, lens);
    CuAssertTrue(tc, 0 != memcmp(origin, data2, lens));
    xor_decode(key, 3, data2, lens);
    CuAssertTrue(tc, 0 == memcmp(origin, data2, lens));

    /* 空数据不崩溃 */
    xor_encode(key, 1, data, 0);
}

/* ======================================================================= */

void test_crypt(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_base64);
    SUITE_ADD_TEST(suite, test_crc);
    SUITE_ADD_TEST(suite, test_digest);
    SUITE_ADD_TEST(suite, test_hmac);
    SUITE_ADD_TEST(suite, test_urlraw);
    SUITE_ADD_TEST(suite, test_xor);
}
