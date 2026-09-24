#include "test_crypt.h"
#include "lib.h"

// SCRAM 用例共用的盐：全文件十来处用同一组值，散着写会让"改一处忘一处"看起来像算法坏了
static const char _SALT16[16] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10
};
static const char _SALT8[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

/* xxhash 官方向量：期望值取自 xxHash tests/sanity_test_vectors.h（按 len、seed 查），
 * 输入为 _xxh_testbuf 生成的数据；XXH3 的 seed 0x9E3779B185EBCA8D 是测试程序的常量，不是算法里的 PRIME64_1 */
#define XXH_TESTBUF_LENS 2367
typedef struct { uint32_t len; uint32_t seed; uint32_t h; } _xxh32_vec;
typedef struct { uint32_t len; uint64_t seed; uint64_t h; } _xxh64_vec;
typedef struct { uint32_t len; uint64_t seed; uint64_t low; uint64_t high; } _xxh128_vec;
static const _xxh32_vec _XXH32_VEC[] = {
    {    0, 0x00000000U, 0x02CC5D05U },
    {    0, 0x9E3779B1U, 0x36B78AE7U },
    {    1, 0x00000000U, 0xCF65B03EU },
    {    1, 0x9E3779B1U, 0xB4545AA4U },
    {    4, 0x00000000U, 0xA9DE7CE9U },
    {    4, 0x9E3779B1U, 0x2BAAFE83U },
    {   14, 0x00000000U, 0x1208E7E2U },
    {   14, 0x9E3779B1U, 0x6AF1D1FEU },
    {   16, 0x00000000U, 0x93BA3759U },
    {   16, 0x9E3779B1U, 0xA94FC1E1U },
    {   17, 0x00000000U, 0x89FDC23EU },
    {   17, 0x9E3779B1U, 0xC9910739U },
    {   32, 0x00000000U, 0xD89829ECU },
    {   32, 0x9E3779B1U, 0xA5C44467U },
    {  222, 0x00000000U, 0x5BD11DBDU },
    {  222, 0x9E3779B1U, 0x58803C5FU },
    { 2367, 0x00000000U, 0x4C8A9773U },
    { 2367, 0x9E3779B1U, 0x6D5366F6U }
};
static const _xxh64_vec _XXH64_VEC[] = {
    {    0, 0x0000000000000000ULL, 0xEF46DB3751D8E999ULL },
    {    0, 0x000000009E3779B1ULL, 0xAC75FDA2929B17EFULL },
    {    1, 0x0000000000000000ULL, 0xE934A84ADB052768ULL },
    {    1, 0x000000009E3779B1ULL, 0x5014607643A9B4C3ULL },
    {    3, 0x0000000000000000ULL, 0xFF7E1959CB50794AULL },
    {    3, 0x000000009E3779B1ULL, 0xAA8584E83660F7D1ULL },
    {    4, 0x0000000000000000ULL, 0x9136A0DCA57457EEULL },
    {    4, 0x000000009E3779B1ULL, 0xCAAB286BD8E9FDB5ULL },
    {    8, 0x0000000000000000ULL, 0xCDBCF538E71D1348ULL },
    {    8, 0x000000009E3779B1ULL, 0xFE0C047A5353CDACULL },
    {   14, 0x0000000000000000ULL, 0x8282DCC4994E35C8ULL },
    {   14, 0x000000009E3779B1ULL, 0xC3BD6BF63DEB6DF0ULL },
    {   16, 0x0000000000000000ULL, 0x98C90B57FDFCB55CULL },
    {   16, 0x000000009E3779B1ULL, 0xC900AD2D536B607EULL },
    {   17, 0x0000000000000000ULL, 0x0D39A2D051A30C2CULL },
    {   17, 0x000000009E3779B1ULL, 0x495CD68A647C7A22ULL },
    {   24, 0x0000000000000000ULL, 0xF75A6DEA42DC5BF4ULL },
    {   24, 0x000000009E3779B1ULL, 0x8B7C67EB59778E22ULL },
    {   32, 0x0000000000000000ULL, 0x18B216492BB44B70ULL },
    {   32, 0x000000009E3779B1ULL, 0xB3F33BDF93ADE409ULL },
    {  222, 0x0000000000000000ULL, 0xB641AE8CB691C174ULL },
    {  222, 0x000000009E3779B1ULL, 0x20CB8AB7AE10C14AULL },
    { 2367, 0x0000000000000000ULL, 0xA82418DDEC0EA581ULL },
    { 2367, 0x000000009E3779B1ULL, 0xA36A93C18052673AULL }
};
static const _xxh64_vec _XXH3_64_VEC[] = {
    {    0, 0x0000000000000000ULL, 0x2D06800538D394C2ULL },
    {    0, 0x9E3779B185EBCA8DULL, 0xA8A6B918B2F0364AULL },
    {    1, 0x0000000000000000ULL, 0xC44BDFF4074EECDBULL },
    {    1, 0x9E3779B185EBCA8DULL, 0x032BE332DD766EF8ULL },
    {    3, 0x0000000000000000ULL, 0x54247382A8D6B94DULL },
    {    3, 0x9E3779B185EBCA8DULL, 0x634B8990B4976373ULL },
    {    4, 0x0000000000000000ULL, 0xE5DC74BC51848A51ULL },
    {    4, 0x9E3779B185EBCA8DULL, 0xAA2E7ECCB0C8F747ULL },
    {    8, 0x0000000000000000ULL, 0x24CCC9ACAA9F65E4ULL },
    {    8, 0x9E3779B185EBCA8DULL, 0x8F973410999B8F6BULL },
    {   12, 0x0000000000000000ULL, 0xA713DAF0DFBB77E7ULL },
    {   12, 0x9E3779B185EBCA8DULL, 0xE7303E1B2336DE0EULL },
    {   16, 0x0000000000000000ULL, 0x981B17D36C7498C9ULL },
    {   16, 0x9E3779B185EBCA8DULL, 0x663F29333B4DB6B1ULL },
    {   17, 0x0000000000000000ULL, 0x796F5ACD3A60F862ULL },
    {   17, 0x9E3779B185EBCA8DULL, 0xF3EC5067F4306DB3ULL },
    {  128, 0x0000000000000000ULL, 0xFCFF24126754D861ULL },
    {  128, 0x9E3779B185EBCA8DULL, 0x73FDE75280646649ULL },
    {  129, 0x0000000000000000ULL, 0x98F1B0A679A2CA29ULL },
    {  129, 0x9E3779B185EBCA8DULL, 0x21FFFDBCA099C844ULL },
    {  240, 0x0000000000000000ULL, 0x81C3C2B67F568CCFULL },
    {  240, 0x9E3779B185EBCA8DULL, 0xCC0F58C27EF3D8EEULL },
    {  241, 0x0000000000000000ULL, 0xC5A639ECD2030E5EULL },
    {  241, 0x9E3779B185EBCA8DULL, 0xDDA9B0A161D4829AULL },
    {  403, 0x0000000000000000ULL, 0xCDEB804D65C6DEA4ULL },
    {  403, 0x9E3779B185EBCA8DULL, 0x6259F6ECFD6443FDULL },
    {  512, 0x0000000000000000ULL, 0x617E49599013CB6BULL },
    {  512, 0x9E3779B185EBCA8DULL, 0x3CE457DE14C27708ULL },
    { 2048, 0x0000000000000000ULL, 0xDD59E2C3A5F038E0ULL },
    { 2048, 0x9E3779B185EBCA8DULL, 0x66F81670669ABABCULL },
    { 2099, 0x0000000000000000ULL, 0xC6B9D9B3FC9AC765ULL },
    { 2099, 0x9E3779B185EBCA8DULL, 0x184F316843663974ULL },
    { 2240, 0x0000000000000000ULL, 0x6E73A90539CF2948ULL },
    { 2240, 0x9E3779B185EBCA8DULL, 0x757BA8487D1B5247ULL },
    { 2367, 0x0000000000000000ULL, 0xCB37AEB9E5D361EDULL },
    { 2367, 0x9E3779B185EBCA8DULL, 0xD2DB3415B942B42AULL }
};
static const _xxh128_vec _XXH3_128_VEC[] = {
    {    0, 0x0000000000000000ULL, 0x6001C324468D497FULL, 0x99AA06D3014798D8ULL },
    {    0, 0x000000009E3779B1ULL, 0x5444F7869C671AB0ULL, 0x92220AE55E14AB50ULL },
    {    0, 0x9E3779B185EBCA8DULL, 0xA986DFC5D7605BFEULL, 0x00FEAA732A3CE25EULL },
    {    1, 0x0000000000000000ULL, 0xC44BDFF4074EECDBULL, 0xA6CD5E9392000F6AULL },
    {    1, 0x000000009E3779B1ULL, 0xB53D5557E7F76F8DULL, 0x89B99554BA22467CULL },
    {    3, 0x0000000000000000ULL, 0x54247382A8D6B94DULL, 0x20EFC49FF02422EAULL },
    {    3, 0x000000009E3779B1ULL, 0xF173D14DAD53A5DCULL, 0x48F82C2FE0ABD468ULL },
    {    4, 0x0000000000000000ULL, 0x2E7D8D6876A39FE9ULL, 0x970D585AC632BF8EULL },
    {    4, 0x000000009E3779B1ULL, 0xEF78D5C489CFE10BULL, 0x7170492A2AA08992ULL },
    {    8, 0x0000000000000000ULL, 0x64C69CAB4BB21DC5ULL, 0x47A7F080D82BB456ULL },
    {    8, 0x000000009E3779B1ULL, 0x5F462F3DE2E8B940ULL, 0xF959013232655FF1ULL },
    {   12, 0x0000000000000000ULL, 0x061A192713F69AD9ULL, 0x6E3EFD8FC7802B18ULL },
    {   12, 0x000000009E3779B1ULL, 0x9BE9F9A67F3C7DFBULL, 0xD7E09D518A3405D3ULL },
    {   16, 0x0000000000000000ULL, 0x562980258A998629ULL, 0xC68C368ECF8A9C05ULL },
    {   16, 0x000000009E3779B1ULL, 0xB07EEEAB4C56392BULL, 0x3767C90D0CDBB93DULL },
    {   17, 0x0000000000000000ULL, 0xABBC12D11973D7DBULL, 0x955FA78643ED3669ULL },
    {   17, 0x000000009E3779B1ULL, 0x3CC9FF6CAE79ACCBULL, 0x99E7C628E75D6431ULL },
    {  128, 0x0000000000000000ULL, 0xEBB15E34A7FB5AB1ULL, 0x39992220E045260AULL },
    {  128, 0x000000009E3779B1ULL, 0x1453819941D93C1DULL, 0x98801187DF8D614DULL },
    {  129, 0x0000000000000000ULL, 0x86C9E3BC8F0A3B5CULL, 0x03815FC91F1B30B6ULL },
    {  129, 0x000000009E3779B1ULL, 0xB37B716F66B40F02ULL, 0xB7F7349A47B39E56ULL },
    {  240, 0x0000000000000000ULL, 0x5C9AAE94C8EBE5A0ULL, 0xAA4202DAA2769DC8ULL },
    {  240, 0x000000009E3779B1ULL, 0xCA19087F1D335DAEULL, 0xDA888104BEAE5AE0ULL },
    {  241, 0x0000000000000000ULL, 0xC5A639ECD2030E5EULL, 0x99A80ECF0ECFC647ULL },
    {  241, 0x000000009E3779B1ULL, 0x5927E3637BAC8149ULL, 0x4BF2229C3A8FC3C3ULL },
    {  403, 0x0000000000000000ULL, 0xCDEB804D65C6DEA4ULL, 0x1B6DE21E332DD73DULL },
    {  403, 0x000000009E3779B1ULL, 0x1FEF87BD75DBE404ULL, 0x1EF41459552CB839ULL },
    {  512, 0x0000000000000000ULL, 0x617E49599013CB6BULL, 0x18D2D110DCC9BCA1ULL },
    {  512, 0x000000009E3779B1ULL, 0x545F610E9F5A78ECULL, 0x06EEB0D56508040FULL },
    { 2048, 0x0000000000000000ULL, 0xDD59E2C3A5F038E0ULL, 0xF736557FD47073A5ULL },
    { 2048, 0x000000009E3779B1ULL, 0x230D43F30206260BULL, 0x7FB03F7E7186C3EAULL },
    { 2048, 0x9E3779B185EBCA8DULL, 0x66F81670669ABABCULL, 0x23CC3A2E75EBAAEAULL },
    { 2367, 0x0000000000000000ULL, 0xCB37AEB9E5D361EDULL, 0xE89C0F6FF369B427ULL },
    { 2367, 0x000000009E3779B1ULL, 0x6F5360AE69C2F406ULL, 0xD23AAE4B76C31ECBULL }
};
// 流式分块大小：覆盖 16/32/256 字节缓冲边界与 1024 字节块边界，最后一项为整块一次 update；
// 3 让补缓冲的 1~3 字节拷贝走到"首、中、尾三字节各不相同"那一档
static const size_t _XXH_CHUNKS[] = { 1, 3, 7, 63, 64, 65, 255, 256, 257, 1024, (size_t)-1 };
static uint8_t _xxh_buf[XXH_TESTBUF_LENS];

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
        const char *plain = cases[i][0];
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
    for (int i = 0; i < 16; i++) {
        bin[i] = (char)i;
    }
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

    /* 空数据：init 值原样出来，只调不断言的话返回垃圾也看不出来 */
    CuAssertTrue(tc, 0x0000 == crc16("", 0));
    CuAssertTrue(tc, 0x00000000 == crc32("", 0));

    /* CRC-32 标准值（IEEE 802.3）*/
    uint32_t c32 = crc32(data, len);
    CuAssertTrue(tc, 0xCBF43926 == c32);

    /* 相同数据结果一致 */
    CuAssertTrue(tc, c16 == crc16(data, len));
    CuAssertTrue(tc, c32 == crc32(data, len));

    /* 不同数据结果不同 */
    CuAssertTrue(tc, c32 != crc32("12345678", len - 1));
}

/* 逐位算的参照实现：CRC-32（反射 0xEDB88320）与 CRC-16/ARC（反射 0xA001） */
static uint32_t _ref_crc32(const uint8_t *p, size_t n) {
    uint32_t c = ~0u;
    int32_t k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}
static uint16_t _ref_crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0;
    int32_t k;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++) {
            c = (uint16_t)((c >> 1) ^ (0xA001u & (0u - (c & 1u))));
        }
    }
    return c;
}
/* crc 按长度走不同分支。查表版：>=64 字节先 4 路并行（每路 8 字节），余下逐 8 字节，再吃 4 字节，最后逐字节；
 * crc32 的硬件版（ARMv8 CRC 指令）逐 8 字节，尾巴按 4/2/1 字节各补一条。一次构建只编其中一版。
 * 长度 0~200 与一段 1027 字节、起点偏移 0~7 逐一对照参照实现，覆盖全部余数与不对齐起点 */
static void test_crc_lengths(CuTest *tc) {
    uint8_t buf[1040];
    size_t i, n, off;
    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(i * 131 + 7);
    }
    for (off = 0; off < 8; off++) {
        for (n = 0; n <= 200; n++) {
            CuAssertTrue(tc, _ref_crc32(buf + off, n) == crc32(buf + off, n));
            CuAssertTrue(tc, _ref_crc16(buf + off, n) == crc16(buf + off, n));
        }
        CuAssertTrue(tc, _ref_crc32(buf + off, 1027) == crc32(buf + off, 1027));
        CuAssertTrue(tc, _ref_crc16(buf + off, 1027) == crc16(buf + off, 1027));
    }
}

/* =======================================================================
 * digest（MD5 / SHA1 / SHA256 / SHA512）
 * ======================================================================= */
static void test_digest(CuTest *tc) {
    char hash[DG_BLOCK_SIZE];
    char hex[HEX_ENSIZE(DG_BLOCK_SIZE)];
    digest_ctx dg;
    size_t hlen;

    /* MD5("") */
    digest_init(&dg, DG_MD5);
    digest_update(&dg, "", 0);
    hlen = digest_final(&dg, hash);
    tohex(hash, hlen, hex, 1);
    CuAssertStrEquals(tc, "d41d8cd98f00b204e9800998ecf8427e", hex);

    /* MD5("abc") */
    digest_init(&dg, DG_MD5);
    digest_update(&dg, "abc", 3);
    hlen = digest_final(&dg, hash);
    tohex(hash, hlen, hex, 1);
    CuAssertStrEquals(tc, "900150983cd24fb0d6963f7d28e17f72", hex);

    /* SHA1("abc") */
    digest_init(&dg, DG_SHA1);
    digest_update(&dg, "abc", 3);
    hlen = digest_final(&dg, hash);
    tohex(hash, hlen, hex, 1);
    CuAssertStrEquals(tc, "a9993e364706816aba3e25717850c26c9cd0d89d", hex);

    /* SHA256("") */
    digest_init(&dg, DG_SHA256);
    digest_update(&dg, "", 0);
    hlen = digest_final(&dg, hash);
    tohex(hash, hlen, hex, 1);
    CuAssertStrEquals(tc, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", hex);

    /* SHA256("abc") */
    digest_init(&dg, DG_SHA256);
    digest_update(&dg, "abc", 3);
    hlen = digest_final(&dg, hash);
    tohex(hash, hlen, hex, 1);
    CuAssertStrEquals(tc, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", hex);

    /* SHA512("abc") */
    const char *sha512abc =
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f";
    digest_init(&dg, DG_SHA512);
    digest_update(&dg, "abc", 3);
    hlen = digest_final(&dg, hash);
    tohex(hash, hlen, hex, 1);
    CuAssertStrEquals(tc, sha512abc, hex);

    /* reset 后可复用：不能只比长度——两个 hlen 都取自同一个 attr.block_lens，
       中间无人动 attr，那条断言等价于 64 == 64，reset 完全不生效也照样通过 */
    digest_reset(&dg);
    digest_update(&dg, "abc", 3);
    size_t hlen2 = digest_final(&dg, hash);
    CuAssertTrue(tc, hlen == hlen2);
    tohex(hash, hlen2, hex, 1);
    CuAssertStrEquals(tc, sha512abc, hex);

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
    char hex[HEX_ENSIZE(DG_BLOCK_SIZE)];
    hmac_ctx hm;
    size_t hlen;

    /* HMAC-SHA256("key", "The quick brown fox jumps over the lazy dog") */
    hmac_init(&hm, DG_SHA256, "key", 3);
    hmac_update(&hm, "The quick brown fox jumps over the lazy dog", 43);
    hlen = hmac_final(&hm, hash);
    tohex(hash, hlen, hex, 1);
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
    url_encode(plain, strlen(plain), enc, 0);
    CuAssertTrue(tc, NULL == strchr(enc, ' '));
    CuAssertTrue(tc, NULL == strchr(enc, '!'));

    /* 解码还原 */
    safe_fill_str(dec, sizeof(dec), enc);
    dlen = url_decode(dec, strlen(dec), 0);
    CuAssertTrue(tc, dlen == strlen(plain));
    CuAssertTrue(tc, 0 == memcmp(dec, plain, dlen));

    /* 纯 ASCII 字母数字不被转义，往返后相同 */
    const char *alpha = "abcABC012";
    url_encode(alpha, strlen(alpha), enc, 0);
    CuAssertStrEquals(tc, alpha, enc);

    /* 空字符串 */
    url_encode("", 0, enc, 0);
    CuAssertTrue(tc, '\0' == enc[0]);
}

/* 逐字符钉住转义集，0x00~0xFF 全部过一遍。实现的保留集是 A-Za-z0-9 加 '-' '.' '_'，比 RFC 3986 的
 * unreserved 少一个 '~'（urlraw.c 的 urlkeep 表）；转义写 %XX 且十六进制大写。
 * 上面那个用例只断言"无空格 / 无 !"与字母数字原样 + 往返，表里错标一格（比如把 : ; < = > ? @
 * 或某个控制字符、高位字节标成原样）照样全过，而 url.encode 是公开 Lua API，编出来的 query 值会被参数注入 */
static void test_url_encode_charset(CuTest *tc) {
    char in[2];
    char out[8];
    char expect[8];
    int32_t c;
    in[1] = '\0';
    for (c = 0x00; c <= 0xFF; c++) {
        in[0] = (char)c;
        url_encode(in, 1, out, 0);
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || '-' == c || '.' == c || '_' == c) {
            expect[0] = (char)c;
            expect[1] = '\0';
        } else {
            SNPRINTF(expect, sizeof(expect), "%%%02X", (uint32_t)c);
        }
        CuAssertStrEquals(tc, expect, out);
    }
    /* space2plus 只改空格这一个字符的去向 */
    in[0] = ' ';
    url_encode(in, 1, out, 1);
    CuAssertStrEquals(tc, "+", out);
    url_encode(in, 1, out, 0);
    CuAssertStrEquals(tc, "%20", out);
    in[0] = '!';
    url_encode(in, 1, out, 1);
    CuAssertStrEquals(tc, "%21", out);
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
    /* 固定输出。xor 是项目自研的链式变换（p[0] 用 key[1..3]，其后每字节
       p[j] = (p[j-1] + p[j]) ^ key[0]，且 p[j-1] 已是变换后的值），没有外部规范可对，
       所以这里钉的是当前行为的金值：只有往返断言的话，两侧一致地改链接方式
       （比如改用未变换的 p[j-1]、或换个 key 下标）恒成立、完全看不出来 */
    static const char golden[] =
        "\x52\xa5\x03\x7d\xfe\x38\x4a\xb3\x30\xb0\x0e\x60\x93\xa1\xe7\x5d"
        "\xd4\x55\x67\xc2\x27\x55\xa4\xd6\x58\xaf\x30\xb6\xc4\x38\xb5\x35"
        "\x47\xad\x0e\x92\xa0\x11\x68\xca\x20\x97\x1b\x5b";
    CuAssertTrue(tc, sizeof(golden) - 1 == lens);
    CuAssertTrue(tc, 0 == memcmp(golden, data, lens));

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

/* =======================================================================
 * SCRAM
 * ======================================================================= */

/* 注入已知 nonce 到客户端，替换 local_nonce 和 local_first_message（用于 RFC 向量测试）*/
static void _scram_inject_nonce(scram_ctx *cli, const char *nonce, const char *user) {
    safe_fill_str(cli->local_nonce, sizeof(cli->local_nonce), nonce);
    FREE(cli->local_first_message);
    cli->local_first_message = format_va("n=%s,r=%s", user, nonce);
}

/* 执行完整双端握手，cli/srv 使用独立的 cbind 数据（用于测试不匹配情形）*/
/* scram_final_message 的返回值含 ClientProof，scram.h 声明处要求擦除后释放、不可裸 FREE。
 * 与 scram.c 的 _scram_free_str 同形：自守 NULL，放在 goto out 的收尾里也安全。
 * first_message 只有用户名和 nonce，不走这里，仍用裸 FREE */
static void _scram_free_msg(char **pmsg) {
    if (NULL != *pmsg) {
        SECURE_FREE(*pmsg, strlen(*pmsg) + 1);
    }
}
static int _scram_handshake(const char *method,
    const char *pwd_cli, const char *pwd_srv,
    const char *cbind_cli, const char *cbind_srv, size_t cbind_len) {
        scram_ctx *cli = scram_init(method, 1);
    scram_ctx *srv = scram_init(method, 0);
    char *cf = NULL, *sf = NULL, *clf = NULL, *svf = NULL;
    int rtn = ERR_FAILED;
    if (!cli || !srv) {
        goto out;
    }

    scram_set_user(cli, "user", 4);
    scram_set_pwd(cli, pwd_cli, strlen(pwd_cli));
    scram_set_pwd(srv, pwd_srv, strlen(pwd_srv));
    if (cbind_cli && cbind_len > 0) {
        scram_set_cbind(cli, cbind_cli, cbind_len);
    }
    if (cbind_srv && cbind_len > 0) {
        scram_set_cbind(srv, cbind_srv, cbind_len);
    }
    scram_set_salt(srv, (char *)_SALT16, sizeof(_SALT16));
    scram_set_iter(srv, 4096);

    cf = scram_first_message(cli);
    if (!cf || ERR_OK != scram_parse_first_message(srv, cf, strlen(cf))) {
        goto out;
    }
    FREE(cf);// FREE 宏自带置 NULL

    sf = scram_first_message(srv);
    if (!sf || ERR_OK != scram_parse_first_message(cli, sf, strlen(sf))) {
        goto out;
    }
    FREE(sf);// FREE 宏自带置 NULL

    clf = scram_final_message(cli);
    if (!clf || ERR_OK != scram_check_final_message(srv, clf, strlen(clf))) {
        goto out;
    }
    _scram_free_msg(&clf);

    svf = scram_final_message(srv);
    if (!svf || ERR_OK != scram_check_final_message(cli, svf, strlen(svf))) {
        goto out;
    }
    _scram_free_msg(&svf);

    /* 握手完成后的期望状态：
     * - 客户端已验证服务端签名 → SCRAM_REMOTE_FINAL
     * - 服务端已发送 v= 消息    → SCRAM_LOCAL_FINAL
     *   （服务端在 scram_check_final_message 时达到 SCRAM_REMOTE_FINAL，
     *    随即在 scram_final_message 生成 v= 后转为 SCRAM_LOCAL_FINAL，
     *    不会再收到对端消息，不会进入第二次 REMOTE_FINAL）*/
    rtn = (SCRAM_REMOTE_FINAL == cli->status && SCRAM_LOCAL_FINAL == srv->status)
          ? ERR_OK : ERR_FAILED;
out:
    FREE(cf); FREE(sf);
    _scram_free_msg(&clf); _scram_free_msg(&svf);
    scram_free(cli);
    scram_free(srv);
    return rtn;
}

/* SHA-1 / SHA-256 / SHA-512 三种算法变体的完整双端握手 */
static void test_scram_handshake(CuTest *tc) {
    const char *methods[] = { "SCRAM-SHA-1", "SCRAM-SHA-256", "SCRAM-SHA-512" };
    for (int i = 0; i < 3; i++) {
        CuAssertTrue(tc, ERR_OK == _scram_handshake(
            methods[i], "correcthorsebatterystaple", "correcthorsebatterystaple",
            NULL, NULL, 0));
    }
    /* 空密码不崩溃，双端空密码可握手 */
    CuAssertTrue(tc, ERR_OK == _scram_handshake(
        "SCRAM-SHA-256", "", "", NULL, NULL, 0));
    /* 握手后服务端能正确获取用户名 */
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        scram_ctx *srv = scram_init("SCRAM-SHA-256", 0);
        scram_set_user(cli, "alice", 5);
        scram_set_pwd(cli, "pass", 4);
        scram_set_pwd(srv, "pass", 4);
        scram_set_salt(srv, (char *)_SALT16, sizeof(_SALT16));
        scram_set_iter(srv, 4096);
        char *cf = scram_first_message(cli);
        scram_parse_first_message(srv, cf, strlen(cf));
        FREE(cf);
        CuAssertStrEquals(tc, "alice", scram_get_user(srv));
        scram_free(cli);
        scram_free(srv);
    }
}

/* RFC 5802（SCRAM-SHA-1）和 RFC 7677（SCRAM-SHA-256）标准测试向量 */
static void test_scram_rfc_vectors(CuTest *tc) {
    /* ── RFC 5802 Section 5：SCRAM-SHA-1 ──────────────────────────────── */
    {
        const char *srv_first =
            "r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,"
            "s=QSXCR+Q6sek8bf92,i=4096";
        const char *exp_cli_final =
            "c=biws,r=fyko+d2lbbFgONRv9qkxdawL3rfcNHYJY1ZVvWVs7j,"
            "p=v0X8v3Bz2T0CJGbJQyF0X+HI4Ts=";
        const char *srv_final = "v=rmF9pqV8S7suAoZWja4dJRkFsKQ=";

        scram_ctx *cli = scram_init("SCRAM-SHA-1", 1);
        CuAssertPtrNotNull(tc, cli);
        scram_set_user(cli, "user", 4);
        scram_set_pwd(cli, "pencil", 6);

        char *first = scram_first_message(cli); // 推进状态至 LOCAL_FIRST
        FREE(first);
        _scram_inject_nonce(cli, "fyko+d2lbbFgONRv9qkxdawL", "user");

        CuAssertIntEquals(tc, ERR_OK,
            scram_parse_first_message(cli, (char *)srv_first, strlen(srv_first)));

        char *cli_final = scram_final_message(cli);
        CuAssertPtrNotNull(tc, cli_final);
        CuAssertStrEquals(tc, exp_cli_final, cli_final);
        _scram_free_msg(&cli_final);

        CuAssertIntEquals(tc, ERR_OK,
            scram_check_final_message(cli, (char *)srv_final, strlen(srv_final)));
        CuAssertIntEquals(tc, SCRAM_REMOTE_FINAL, (int)cli->status);

        scram_free(cli);
    }

    /* ── RFC 7677 Section 3：SCRAM-SHA-256 ────────────────────────────── */
    {
        const char *srv_first =
            "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
            "s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";
        const char *exp_cli_final =
            "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
            "p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=";
        const char *srv_final = "v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=";

        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        CuAssertPtrNotNull(tc, cli);
        scram_set_user(cli, "user", 4);
        scram_set_pwd(cli, "pencil", 6);

        char *first = scram_first_message(cli);
        FREE(first);
        _scram_inject_nonce(cli, "rOprNGfwEbeRWgbNEkqO", "user");

        CuAssertIntEquals(tc, ERR_OK,
            scram_parse_first_message(cli, (char *)srv_first, strlen(srv_first)));

        char *cli_final = scram_final_message(cli);
        CuAssertPtrNotNull(tc, cli_final);
        CuAssertStrEquals(tc, exp_cli_final, cli_final);
        _scram_free_msg(&cli_final);

        CuAssertIntEquals(tc, ERR_OK,
            scram_check_final_message(cli, (char *)srv_final, strlen(srv_final)));
        CuAssertIntEquals(tc, SCRAM_REMOTE_FINAL, (int)cli->status);

        scram_free(cli);
    }
}

/* SCRAM-SHA-256-PLUS：channel binding 正常握手及 GS2 头验证 */
static void test_scram_plus(CuTest *tc) {
    /* 模拟 tls-server-end-point：服务端证书 SHA-256 哈希（32 字节）*/
    const char cbind[32] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
    };

    /* 正常 PLUS 握手 */
    CuAssertTrue(tc, ERR_OK == _scram_handshake(
        "SCRAM-SHA-256-PLUS", "pass", "pass", cbind, cbind, sizeof(cbind)));

    /* 客户端第一条消息必须以 PLUS GS2 头开始 */
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256-PLUS", 1);
        CuAssertPtrNotNull(tc, cli);
        scram_set_user(cli, "user", 4);
        scram_set_pwd(cli, "pass", 4);
        scram_set_cbind(cli, cbind, sizeof(cbind));
        char *first = scram_first_message(cli);
        CuAssertPtrNotNull(tc, first);
        CuAssertTrue(tc, 0 == strncmp(first, "p=tls-server-end-point,,", 24));
        FREE(first);
        scram_free(cli);
    }

    /* 三种 PLUS 变体均可握手 */
    const char *plus_methods[] = {
        "SCRAM-SHA-1-PLUS", "SCRAM-SHA-256-PLUS", "SCRAM-SHA-512-PLUS"
    };
    for (int i = 0; i < 3; i++) {
        CuAssertTrue(tc, ERR_OK == _scram_handshake(
            plus_methods[i], "pass", "pass", cbind, cbind, sizeof(cbind)));
    }
}

/* PLUS 变体漏调 scram_set_cbind 时不能按"绑定到零字节"算：两端都缺数据的话，
 * 两边算出的 c= 反而对得上，握手照常成功，双方都以为通道绑定生效 */
static void test_scram_plus_requires_cbind(CuTest *tc) {
    const char cbind[32] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
    };

    /* 两端都没设：改前这一条会握手成功 */
    CuAssertTrue(tc, ERR_OK != _scram_handshake(
        "SCRAM-SHA-256-PLUS", "pass", "pass", NULL, NULL, 0));
    /* 只有一端设，同样必须失败 */
    CuAssertTrue(tc, ERR_OK != _scram_handshake(
        "SCRAM-SHA-256-PLUS", "pass", "pass", cbind, NULL, sizeof(cbind)));
    CuAssertTrue(tc, ERR_OK != _scram_handshake(
        "SCRAM-SHA-256-PLUS", "pass", "pass", NULL, cbind, sizeof(cbind)));

    /* 客户端缺数据时在自己的 final 就返 NULL，不用等服务端算出失配 */
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256-PLUS", 1);
        scram_ctx *srv = scram_init("SCRAM-SHA-256-PLUS", 0);
        CuAssertPtrNotNull(tc, cli);
        CuAssertPtrNotNull(tc, srv);
        scram_set_user(cli, "user", 4);
        scram_set_pwd(cli, "pass", 4);
        scram_set_pwd(srv, "pass", 4);
        scram_set_salt(srv, (char *)_SALT8, sizeof(_SALT8));
        scram_set_iter(srv, 4096);
        scram_set_cbind(srv, cbind, sizeof(cbind));

        char *cf = scram_first_message(cli);
        CuAssertPtrNotNull(tc, cf);
        CuAssertIntEquals(tc, ERR_OK, scram_parse_first_message(srv, cf, strlen(cf)));
        char *sf = scram_first_message(srv);
        CuAssertPtrNotNull(tc, sf);
        CuAssertIntEquals(tc, ERR_OK, scram_parse_first_message(cli, sf, strlen(sf)));
        CuAssertTrue(tc, NULL == scram_final_message(cli));

        FREE(cf);
        FREE(sf);
        scram_free(cli);
        scram_free(srv);
    }
}

// 伪造只有 i= 值不同的服务端首条消息（nonce 前缀取自客户端以绕过 nonce 校验），
// 返回 scram_parse_first_message 的结果；outiter 非空时回带解析后的迭代轮数
static int32_t _scram_parse_iter(CuTest *tc, const char *iter, int32_t *outiter) {
    scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
    CuAssertPtrNotNull(tc, cli);
    scram_set_pwd(cli, "pass", 4);
    char *first = scram_first_message(cli);
    CuAssertPtrNotNull(tc, first);
    FREE(first);
    char fake_srv[256];
    SNPRINTF(fake_srv, sizeof(fake_srv),
        "r=%sFAKESUFFIX,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=%s", cli->local_nonce, iter);
    int32_t ret = scram_parse_first_message(cli, fake_srv, strlen(fake_srv));
    if (NULL != outiter) {
        *outiter = cli->iter;
    }
    scram_free(cli);
    return ret;
}
/* 各类失败情形 */
/* -----------------------------------------------------------------------
 * scram —— 非 PLUS 服务端与发 "y,," 的客户端跑完整握手。
 * RFC 5802 §5 规定客户端支持 channel binding 但未见到 -PLUS 通告时必须发 "y"，
 * 此时服务端应正常继续；而 c= 是 base64(GS2头)，服务端必须按对端实际发来的头重算，
 * 若按自身配置的 "n,," 重算则 c= 永不匹配，且失败与密码错误不可区分。
 * 若 _scram_cbind_b64 被改回按 cbind 选常量，服务端会算出 base64("n,,")
 * 而与客户端的 base64("y,,") 失配，本用例即失败
 * ----------------------------------------------------------------------- */
static void test_scram_gs2_y_handshake(CuTest *tc) {
    scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
    scram_ctx *srv = scram_init("SCRAM-SHA-256", 0);
    CuAssertPtrNotNull(tc, cli);
    CuAssertPtrNotNull(tc, srv);
    scram_set_user(cli, "user", 4);
    scram_set_pwd(cli, "pencil", 6);
    scram_set_pwd(srv, "pencil", 6);
    scram_set_salt(srv, (char *)_SALT8, sizeof(_SALT8));
    scram_set_iter(srv, 4096);
    // 非 PLUS 客户端拿到绑定材料 → 转 CAPABLE，GS2 头发 "y,,"
    char cb[32] = { 0x5a };
    CuAssertIntEquals(tc, ERR_OK, scram_set_cbind(cli, cb, sizeof(cb)));

    char *clf = scram_first_message(cli);
    CuAssertPtrNotNull(tc, clf);
    CuAssertIntEquals(tc, 'y', clf[0]);
    CuAssertStrEquals(tc, "y,,", cli->gs2_header);
    CuAssertIntEquals(tc, ERR_OK, scram_parse_first_message(srv, clf, strlen(clf)));
    CuAssertStrEquals(tc, "y,,", srv->gs2_header);

    char *svf = scram_first_message(srv);
    CuAssertPtrNotNull(tc, svf);
    CuAssertIntEquals(tc, ERR_OK, scram_parse_first_message(cli, svf, strlen(svf)));

    char *clfin = scram_final_message(cli);
    CuAssertPtrNotNull(tc, clfin);
    CuAssertIntEquals(tc, ERR_OK, scram_check_final_message(srv, clfin, strlen(clfin)));

    char *svfin = scram_final_message(srv);
    CuAssertPtrNotNull(tc, svfin);
    CuAssertIntEquals(tc, ERR_OK, scram_check_final_message(cli, svfin, strlen(svfin)));

    FREE(clf);
    FREE(svf);
    _scram_free_msg(&clfin);
    _scram_free_msg(&svfin);
    scram_free(cli);
    scram_free(srv);
}

// scram_set_cbind 的三态分流：PLUS 存数据、非 PLUS 转 CAPABLE、空数据一律拒；
// 以及非 PLUS 那条的状态守卫——首条消息一发 GS2 头就定了，再设来不及
static void test_scram_cbind_modes(CuTest *tc) {
    char cb[8] = { 7, 7, 7, 7, 7, 7, 7, 7 };

    // PLUS：数据存进 cbind_data，姿态不变
    scram_ctx *plus = scram_init("SCRAM-SHA-256-PLUS", 1);
    CuAssertPtrNotNull(tc, plus);
    CuAssertIntEquals(tc, SCRAM_CB_PLUS, plus->cbind);
    CuAssertIntEquals(tc, ERR_FAILED, scram_set_cbind(plus, NULL, sizeof(cb)));
    CuAssertIntEquals(tc, ERR_FAILED, scram_set_cbind(plus, cb, 0));
    CuAssertTrue(tc, NULL == plus->cbind_data);
    CuAssertIntEquals(tc, ERR_OK, scram_set_cbind(plus, cb, sizeof(cb)));
    CuAssertIntEquals(tc, 8, plus->cbind_len);
    CuAssertIntEquals(tc, SCRAM_CB_PLUS, plus->cbind);
    scram_free(plus);

    // 非 PLUS：数据用不上但姿态升到 CAPABLE，两个角色都适用（服务端靠它拒 "y"）
    scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
    CuAssertPtrNotNull(tc, cli);
    CuAssertIntEquals(tc, SCRAM_CB_NONE, cli->cbind);
    CuAssertIntEquals(tc, ERR_OK, scram_set_cbind(cli, cb, sizeof(cb)));
    CuAssertIntEquals(tc, SCRAM_CB_CAPABLE, cli->cbind);
    CuAssertTrue(tc, NULL == cli->cbind_data);// 数据没被存
    scram_free(cli);

    scram_ctx *srv = scram_init("SCRAM-SHA-256", 0);
    CuAssertPtrNotNull(tc, srv);
    CuAssertIntEquals(tc, ERR_OK, scram_set_cbind(srv, cb, sizeof(cb)));
    CuAssertIntEquals(tc, SCRAM_CB_CAPABLE, srv->cbind);
    scram_free(srv);

    // 状态守卫：首条消息发过之后再设无效，默认仍是 "n,,"
    scram_ctx *late = scram_init("SCRAM-SHA-256", 1);
    CuAssertPtrNotNull(tc, late);
    scram_set_user(late, "user", 4);
    scram_set_pwd(late, "pass", 4);
    char *first = scram_first_message(late);
    CuAssertPtrNotNull(tc, first);
    CuAssertIntEquals(tc, 'n', first[0]);
    CuAssertStrEquals(tc, "n,,", late->gs2_header);
    CuAssertIntEquals(tc, ERR_FAILED, scram_set_cbind(late, cb, sizeof(cb)));
    CuAssertIntEquals(tc, SCRAM_CB_NONE, late->cbind);
    FREE(first);
    scram_free(late);
}

// RFC 5802 §6：本端也通告了 -PLUS（CAPABLE）却收到 "y"，说明通告在路上被剥，必须拒绝握手。
// 对照组是同一条消息发给未通告 -PLUS 的服务端（NONE），那里 "y" 是合法的、须照常放行
static void test_scram_server_reject_downgrade(CuTest *tc) {
    char cb[32] = { 0x5a };
    const char *ymsg = "y,,n=user,r=abcdefghijklmnop";
    size_t ylens = strlen(ymsg);
    char buf[64];

    scram_ctx *plusadv = scram_init("SCRAM-SHA-256", 0);
    CuAssertPtrNotNull(tc, plusadv);
    CuAssertIntEquals(tc, ERR_OK, scram_set_cbind(plusadv, cb, sizeof(cb)));
    memcpy(buf, ymsg, ylens + 1);
    CuAssertIntEquals(tc, ERR_FAILED, scram_parse_first_message(plusadv, buf, ylens));
    scram_free(plusadv);

    scram_ctx *plain = scram_init("SCRAM-SHA-256", 0);
    CuAssertPtrNotNull(tc, plain);
    CuAssertIntEquals(tc, SCRAM_CB_NONE, plain->cbind);
    memcpy(buf, ymsg, ylens + 1);
    CuAssertIntEquals(tc, ERR_OK, scram_parse_first_message(plain, buf, ylens));
    CuAssertStrEquals(tc, "y,,", plain->gs2_header);
    scram_free(plain);
}

// 服务端解析 client-first-message 须按实际内容定位 GS2 头，而不是按自身配置推算长度：
// 客户端可以合法发来别的 flag，按固定长度硬切会切错 bare 消息却仍返 ERR_OK；
// PLUS 端收到 "y" 是降级攻击（RFC 5802 §6），authzid 非空本端无法遵从，二者都须拒绝
static void test_scram_gs2_header(CuTest *tc) {
    const struct { const char *method; const char *msg; int32_t want; const char *gs2; } cases[] = {
        { "SCRAM-SHA-256", "n,,n=user,r=abcdefghijklmnop", ERR_OK, "n,," },
        { "SCRAM-SHA-256", "y,,n=user,r=abcdefghijklmnop", ERR_OK, "y,," },
        { "SCRAM-SHA-256", "p=tls-server-end-point,,n=user,r=abcdefghijklmnop", ERR_FAILED, NULL },
        { "SCRAM-SHA-256", "n,a=other,n=user,r=abcdefghijklmnop", ERR_FAILED, NULL },
        { "SCRAM-SHA-256", "n,", ERR_FAILED, NULL },
        { "SCRAM-SHA-256", "z,,n=user,r=abcdefghijklmnop", ERR_FAILED, NULL },
        { "SCRAM-SHA-256-PLUS", "p=tls-server-end-point,,n=user,r=abcdefghijklmnop", ERR_OK, "p=tls-server-end-point,," },
        { "SCRAM-SHA-256-PLUS", "y,,n=user,r=abcdefghijklmnop", ERR_FAILED, NULL },
        { "SCRAM-SHA-256-PLUS", "n,,n=user,r=abcdefghijklmnop", ERR_FAILED, NULL },
        { "SCRAM-SHA-256-PLUS", "p=tls-unique,,n=user,r=abcdefghijklmnop", ERR_FAILED, NULL }
    };
    char msg[128];
    scram_ctx *srv;
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        SNPRINTF(msg, sizeof(msg), "%s", cases[i].msg);
        srv = scram_init(cases[i].method, 0);
        CuAssertPtrNotNull(tc, srv);
        CuAssertIntEquals(tc, cases[i].want, scram_parse_first_message(srv, msg, strlen(msg)));
        if (NULL != cases[i].gs2) {
            CuAssertStrEquals(tc, cases[i].gs2, srv->gs2_header);
        }
        scram_free(srv);
    }
}

// client-first 的属性校验：SASLname 的 '=' 转义只认 =2C/=3D，裸 '=' 一律拒（否则
// "victim=41" 与合法编码 "victim=3D41" 会归一成同一个身份）；r= 不得为空；
// m= 是 RFC 5802 §5 的强制扩展，本端不实现就必须失败
static void test_scram_client_first_attrs(CuTest *tc) {
    const struct { const char *msg; int32_t want; const char *user; } cases[] = {
        { "n,,n=user,r=abcdefghijklmnop",            ERR_OK,     "user" },
        { "n,,n=user=3D41,r=abcdefghijklmnop",       ERR_OK,     "user=41" },
        { "n,,n=user=2C=3Dtest,r=abcdefghijklmnop",  ERR_OK,     "user,=test" },
        { "n,,n=victim=41,r=abcdefghijklmnop",       ERR_FAILED, NULL },
        { "n,,n=user=2,r=abcdefghijklmnop",          ERR_FAILED, NULL },
        { "n,,n=user,r=",                            ERR_FAILED, NULL },
        { "n,,m=needthis,n=user,r=abcdefghijklmnop", ERR_FAILED, NULL }
    };
    char msg[128];
    scram_ctx *srv;
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        SNPRINTF(msg, sizeof(msg), "%s", cases[i].msg);
        srv = scram_init("SCRAM-SHA-256", 0);
        CuAssertPtrNotNull(tc, srv);
        CuAssertIntEquals(tc, cases[i].want, scram_parse_first_message(srv, msg, strlen(msg)));
        if (NULL != cases[i].user) {
            CuAssertStrEquals(tc, cases[i].user, scram_get_user(srv));
        }
        scram_free(srv);
    }
}

static void test_scram_failures(CuTest *tc) {
    /* 不支持的方法 → NULL */
    CuAssertTrue(tc, NULL == scram_init("SCRAM-MD5", 1));
    CuAssertTrue(tc, NULL == scram_init("", 0));
    /* 机制名按表查、-PLUS 后缀单独剥：这几个都不许被当成合法名收下 */
    CuAssertTrue(tc, NULL == scram_init("-PLUS", 1));
    CuAssertTrue(tc, NULL == scram_init("SCRAM-SHA-1-PLUSX", 1));
    CuAssertTrue(tc, NULL == scram_init("SCRAM-SHA-1X", 1));
    CuAssertTrue(tc, NULL == scram_init("SCRAM-SHA-", 1));
    CuAssertTrue(tc, NULL == scram_init("SCRAM-SHA-1-PLUS-PLUS", 1));
    /* 三种机制的两个变体都认，且 -PLUS 必须落到 SCRAM_CB_PLUS */
    static const char *const ok[] = { "SCRAM-SHA-1", "SCRAM-SHA-256", "SCRAM-SHA-512" };
    char plus[64];
    scram_ctx *sc;
    for (size_t i = 0; i < ARRAY_SIZE(ok); i++) {
        sc = scram_init(ok[i], 1);
        CuAssertPtrNotNull(tc, sc);
        CuAssertIntEquals(tc, SCRAM_CB_NONE, sc->cbind);
        scram_free(sc);
        SNPRINTF(plus, sizeof(plus), "%s-PLUS", ok[i]);
        sc = scram_init(plus, 1);
        CuAssertPtrNotNull(tc, sc);
        CuAssertIntEquals(tc, SCRAM_CB_PLUS, sc->cbind);
        scram_free(sc);
    }

    /* 密码不匹配 → 服务端拒绝客户端证明 */
    CuAssertTrue(tc, ERR_OK != _scram_handshake(
        "SCRAM-SHA-256", "right", "wrong", NULL, NULL, 0));

    CuAssert(tc, "i below lower bound must be rejected", ERR_OK != _scram_parse_iter(tc, "100", NULL));
    CuAssert(tc, "i = INT32_MAX must be rejected", ERR_OK != _scram_parse_iter(tc, "2147483647", NULL));
    int32_t iter = 0;
    CuAssert(tc, "i within valid range must be accepted", ERR_OK == _scram_parse_iter(tc, "40960", &iter));
    CuAssertIntEquals(tc, 40960, iter);
    CuAssert(tc, "i exactly SCRAM_MAX_ITER(256 * 4096 = 1048576) must be accepted",
        ERR_OK == _scram_parse_iter(tc, "1048576", &iter));
    CuAssertIntEquals(tc, 1048576, iter);
    CuAssert(tc, "i one round over SCRAM_MAX_ITER must be rejected",
        ERR_OK != _scram_parse_iter(tc, "1048577", NULL));

    /* 服务端签名被篡改 → 客户端拒绝 */
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        scram_ctx *srv = scram_init("SCRAM-SHA-256", 0);
        scram_set_user(cli, "user", 4);
        scram_set_pwd(cli, "pass", 4);
        scram_set_pwd(srv, "pass", 4);
        scram_set_salt(srv, (char *)_SALT16, sizeof(_SALT16));
        scram_set_iter(srv, 4096);

        char *cf = scram_first_message(cli);
        CuAssertPtrNotNull(tc, cf);
        scram_parse_first_message(srv, cf, strlen(cf)); FREE(cf);
        char *sf = scram_first_message(srv);
        CuAssertPtrNotNull(tc, sf);
        scram_parse_first_message(cli, sf, strlen(sf)); FREE(sf);
        char *clf = scram_final_message(cli);
        CuAssertPtrNotNull(tc, clf);
        scram_check_final_message(srv, clf, strlen(clf)); _scram_free_msg(&clf);

        char *svf = scram_final_message(srv);
        CuAssertPtrNotNull(tc, svf);
        svf[2]++; /* 篡改签名首字节 */
        CuAssertTrue(tc, ERR_OK != scram_check_final_message(cli, svf, strlen(svf)));
        _scram_free_msg(&svf);
        scram_free(cli);
        scram_free(srv);
    }

    /* 状态机：未完成首轮交换就调用 scram_final_message → NULL */
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        scram_set_pwd(cli, "pass", 4);
        /* status=INIT → NULL */
        CuAssertTrue(tc, NULL == scram_final_message(cli));
        char *first = scram_first_message(cli);
        FREE(first);
        /* status=LOCAL_FIRST，未解析服务端消息 → NULL */
        CuAssertTrue(tc, NULL == scram_final_message(cli));
        scram_free(cli);
    }

    /* 非 PLUS 变体调用 scram_set_cbind：数据不入 cbind_data，只把姿态升到 CAPABLE */
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        CuAssertIntEquals(tc, ERR_OK, scram_set_cbind(cli, "binddata", 8));
        CuAssertIntEquals(tc, SCRAM_CB_CAPABLE, cli->cbind);
        CuAssertTrue(tc, NULL == cli->cbind_data);
        scram_free(cli);
    }

    /* PLUS 变体 cbind_data 不匹配 → 服务端拒绝 */
    {
        const char cbind_a[8] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22 };
        const char cbind_b[8] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };
        CuAssertTrue(tc, ERR_OK != _scram_handshake(
            "SCRAM-SHA-256-PLUS", "pass", "pass", cbind_a, cbind_b, sizeof(cbind_a)));
    }

    /* 用户名含 ',' 和 '='：消息中正确转义，服务端正确还原 */
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        scram_ctx *srv = scram_init("SCRAM-SHA-256", 0);
        scram_set_user(cli, "user,=test", 10);
        scram_set_pwd(cli, "pass", 4);
        scram_set_pwd(srv, "pass", 4);
        scram_set_salt(srv, (char *)_SALT16, sizeof(_SALT16));
        scram_set_iter(srv, 4096);

        char *first = scram_first_message(cli);
        CuAssertPtrNotNull(tc, first);
        /* 转义后消息中含 '=2C'（逗号）和 '=3D'（等号）*/
        CuAssertTrue(tc, NULL != strstr(first, "n=user=2C=3Dtest"));
        CuAssertIntEquals(tc, ERR_OK,
            scram_parse_first_message(srv, first, strlen(first)));
        /* 服务端正确还原用户名 */
        CuAssertStrEquals(tc, "user,=test", scram_get_user(srv));
        FREE(first);
        scram_free(cli);
        scram_free(srv);
    }
}

// scram setter 角色拒绝路径 + scram_set_pwd 动态分配验证 + scram_free(NULL) NULL safety。
// 五个 setter 改返 int32_t 后每个拒绝分支都断言返回码；末段 set_cbind 覆盖非 PLUS 变体被拒、
// PLUS 变体空数据被拒、PLUS 变体正常数据生效三态
static void test_scram_setters(CuTest *tc) {
    // scram_free(NULL) 不崩
    scram_free(NULL);

    // scram_set_pwd：511 字节正常设置（动态分配，无长度上限）
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        CuAssertPtrNotNull(tc, cli);
        char pwd511[512];
        memset(pwd511, 'x', 511);
        pwd511[511] = '\0';
        CuAssertIntEquals(tc, ERR_OK, scram_set_pwd(cli, pwd511, 511));
        CuAssertIntEquals(tc, 511, (int)strlen(cli->pwd));
        scram_free(cli);
    }
    // scram_set_pwd：512 字节同样可设置
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        char pwd512[513];
        memset(pwd512, 'x', 512);
        pwd512[512] = '\0';
        scram_set_pwd(cli, pwd512, 512);
        CuAssertIntEquals(tc, 512, (int)strlen(cli->pwd));
        scram_free(cli);
    }
    // scram_set_iter：服务端 iter < SCRAM_MIN_ITER(4096) 自动提升到 4096
    {
        scram_ctx *srv = scram_init("SCRAM-SHA-256", 0);
        scram_set_iter(srv, 100);
        CuAssertIntEquals(tc, 4096, srv->iter);
        scram_set_iter(srv, 10000);
        CuAssertIntEquals(tc, 10000, srv->iter);
        // 高于 SCRAM_MAX_ITER(256 * 4096) 夹到上限,与客户端解析服务端 i= 时的上限同值,
        // 所以夹过的值对端照样接受
        scram_set_iter(srv, INT32_MAX);
        CuAssertIntEquals(tc, 256 * 4096, srv->iter);
        scram_free(srv);
    }
    // scram_set_iter：客户端调用被拒（iter 保持 0）
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        CuAssertIntEquals(tc, ERR_FAILED, scram_set_iter(cli, 8192));
        CuAssertIntEquals(tc, 0, cli->iter);
        scram_free(cli);
    }
    // 服务端漏调 set_salt / set_iter：必须硬失败，不能静默发出 "s=,i=0"
    // 让 PBKDF2 退化成无盐单轮（与 pwd / PLUS cbind 那两道守卫同形）
    {
        char cmsg[] = "n,,n=admin,r=Ym9ndXNub25jZQ==";
        // 两个都没设
        scram_ctx *s0 = scram_init("SCRAM-SHA-256", 0);
        CuAssertIntEquals(tc, ERR_OK, scram_parse_first_message(s0, cmsg, sizeof(cmsg) - 1));
        CuAssertTrue(tc, NULL == scram_first_message(s0));
        scram_free(s0);
        // 只设了 iter，缺 salt
        scram_ctx *s1 = scram_init("SCRAM-SHA-256", 0);
        CuAssertIntEquals(tc, ERR_OK, scram_parse_first_message(s1, cmsg, sizeof(cmsg) - 1));
        scram_set_iter(s1, 4096);
        CuAssertTrue(tc, NULL == scram_first_message(s1));
        scram_free(s1);
        // 只设了 salt，缺 iter
        scram_ctx *s2 = scram_init("SCRAM-SHA-256", 0);
        CuAssertIntEquals(tc, ERR_OK, scram_parse_first_message(s2, cmsg, sizeof(cmsg) - 1));
        scram_set_salt(s2, (char *)_SALT8, sizeof(_SALT8));
        CuAssertTrue(tc, NULL == scram_first_message(s2));
        scram_free(s2);
        // 两个都设齐即放行
        scram_ctx *s3 = scram_init("SCRAM-SHA-256", 0);
        CuAssertIntEquals(tc, ERR_OK, scram_parse_first_message(s3, cmsg, sizeof(cmsg) - 1));
        scram_set_salt(s3, (char *)_SALT8, sizeof(_SALT8));
        scram_set_iter(s3, 4096);
        char *ok = scram_first_message(s3);
        CuAssertPtrNotNull(tc, ok);
        FREE(ok);
        scram_free(s3);
    }
    // scram_set_salt：客户端调用被拒
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        CuAssertIntEquals(tc, ERR_FAILED, scram_set_salt(cli, (char *)_SALT8, sizeof(_SALT8)));
        CuAssertTrue(tc, NULL == cli->salt);
        CuAssertIntEquals(tc, 0, cli->saltlen);
        scram_free(cli);
    }
    // scram_set_salt：服务端 NULL 或 0 长度被拒
    {
        scram_ctx *srv = scram_init("SCRAM-SHA-256", 0);
        CuAssertIntEquals(tc, ERR_FAILED, scram_set_salt(srv, NULL, 8));
        CuAssertTrue(tc, NULL == srv->salt);
        char salt4[4] = { 9, 9, 9, 9 };
        CuAssertIntEquals(tc, ERR_FAILED, scram_set_salt(srv, salt4, 0));
        CuAssertTrue(tc, NULL == srv->salt);
        // 正常路径：本段专验长度原样落库，故用一个与 _SALT8 长度不同的盐
        CuAssertIntEquals(tc, ERR_OK, scram_set_salt(srv, salt4, sizeof(salt4)));
        CuAssertPtrNotNull(tc, srv->salt);
        CuAssertIntEquals(tc, 4, srv->saltlen);
        // 再次设置覆盖之前
        char salt2[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        CuAssertIntEquals(tc, ERR_OK, scram_set_salt(srv, salt2, sizeof(salt2)));
        CuAssertIntEquals(tc, 8, srv->saltlen);
        scram_free(srv);
    }
    {
        // 非 PLUS 不再是"被拒"，而是转成 CAPABLE；三态分流细节见 test_scram_cbind_modes
        scram_ctx *std = scram_init("SCRAM-SHA-256", 1);
        char cb[4] = { 7, 7, 7, 7 };
        CuAssertIntEquals(tc, ERR_OK, scram_set_cbind(std, cb, sizeof(cb)));
        CuAssertIntEquals(tc, SCRAM_CB_CAPABLE, std->cbind);
        CuAssertTrue(tc, NULL == std->cbind_data);
        scram_free(std);
        scram_ctx *plus = scram_init("SCRAM-SHA-256-PLUS", 1);
        CuAssertIntEquals(tc, ERR_FAILED, scram_set_cbind(plus, NULL, sizeof(cb)));
        CuAssertIntEquals(tc, ERR_FAILED, scram_set_cbind(plus, cb, 0));
        CuAssertTrue(tc, NULL == plus->cbind_data);
        CuAssertIntEquals(tc, ERR_OK, scram_set_cbind(plus, cb, sizeof(cb)));
        CuAssertIntEquals(tc, 4, plus->cbind_len);
        scram_free(plus);
    }
    // scram_set_user：服务端也可直接调用
    {
        scram_ctx *srv = scram_init("SCRAM-SHA-256", 0);
        CuAssertIntEquals(tc, ERR_OK, scram_set_user(srv, "alice", 5));
        CuAssertStrEquals(tc, "alice", srv->user);
        scram_free(srv);
    }
    // scram_set_user：任意长度均可设置（动态分配）
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        char longuser[128];
        memset(longuser, 'A', 127);
        longuser[127] = '\0';
        scram_set_user(cli, longuser, 127);
        CuAssertIntEquals(tc, 127, (int)strlen(cli->user));
        scram_free(cli);
    }
}

/* -----------------------------------------------------------------------
 * scram —— 从未 scram_set_pwd 时两条最终消息路径都失败返回，而非解引用 NULL。
 * 前半服务端有 salt/iter 但无密码，校验客户端证明须返 ERR_FAILED——这一路由远端消息触达；
 * 后半客户端走完首轮交换但无密码，scram_final_message 须返 NULL。
 * 两处原先都落到 _scram_salt_password 的 strlen(NULL) 上
 * ----------------------------------------------------------------------- */
static void test_scram_pwd_required(CuTest *tc) {

    scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
    scram_ctx *srv = scram_init("SCRAM-SHA-256", 0);
    CuAssertPtrNotNull(tc, cli);
    CuAssertPtrNotNull(tc, srv);
    scram_set_user(cli, "user", 4);
    scram_set_pwd(cli, "pass", 4);
    scram_set_salt(srv, (char *)_SALT8, sizeof(_SALT8));
    scram_set_iter(srv, 4096);
    char *clf = scram_first_message(cli);
    CuAssertPtrNotNull(tc, clf);
    CuAssertIntEquals(tc, ERR_OK, scram_parse_first_message(srv, clf, strlen(clf)));
    char *svf = scram_first_message(srv);
    CuAssertPtrNotNull(tc, svf);
    CuAssertIntEquals(tc, ERR_OK, scram_parse_first_message(cli, svf, strlen(svf)));
    char *clfin = scram_final_message(cli);
    CuAssertPtrNotNull(tc, clfin);
    CuAssertTrue(tc, ERR_OK != scram_check_final_message(srv, clfin, strlen(clfin)));
    FREE(clf);
    FREE(svf);
    _scram_free_msg(&clfin);
    scram_free(cli);
    scram_free(srv);

    scram_ctx *cli2 = scram_init("SCRAM-SHA-256", 1);
    scram_ctx *srv2 = scram_init("SCRAM-SHA-256", 0);
    CuAssertPtrNotNull(tc, cli2);
    CuAssertPtrNotNull(tc, srv2);
    scram_set_user(cli2, "user", 4);
    scram_set_pwd(srv2, "pass", 4);
    scram_set_salt(srv2, (char *)_SALT8, sizeof(_SALT8));
    scram_set_iter(srv2, 4096);
    char *clf2 = scram_first_message(cli2);
    CuAssertPtrNotNull(tc, clf2);
    CuAssertIntEquals(tc, ERR_OK, scram_parse_first_message(srv2, clf2, strlen(clf2)));
    char *svf2 = scram_first_message(srv2);
    CuAssertPtrNotNull(tc, svf2);
    CuAssertIntEquals(tc, ERR_OK, scram_parse_first_message(cli2, svf2, strlen(svf2)));
    CuAssertTrue(tc, NULL == scram_final_message(cli2));
    FREE(clf2);
    FREE(svf2);
    scram_free(cli2);
    scram_free(srv2);
}

/* -----------------------------------------------------------------------
 * scram —— 内嵌 NUL 的用户名/密码在三个入口整体拒绝且不改动原值。
 * 下游 PBKDF2 与 secure_zero 一律按 strlen 取长，放行则 "ab\0cd" 与 "ab\0xy" 派生出
 * 同一 ClientProof、NUL 之后的密钥尾字节还会留在释放后的堆上；服务端线路解析放行
 * 则把 "ad\0in" 记成 "ad" 这另一个身份。每组先跑同一条消息的无 NUL 版本做正对照，
 * 确认拒绝来自 NUL 本身而不是别的畸形
 * ----------------------------------------------------------------------- */
static void test_scram_embedded_nul(CuTest *tc) {
    scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
    CuAssertPtrNotNull(tc, cli);
    CuAssertIntEquals(tc, ERR_FAILED, scram_set_pwd(cli, "ab\0cd", 5));
    CuAssertTrue(tc, NULL == cli->pwd);
    CuAssertIntEquals(tc, ERR_OK, scram_set_pwd(cli, "abcd", 4));
    CuAssertStrEquals(tc, "abcd", cli->pwd);
    CuAssertIntEquals(tc, ERR_FAILED, scram_set_pwd(cli, "ab\0cd", 5));
    CuAssertStrEquals(tc, "abcd", cli->pwd);
    CuAssertIntEquals(tc, ERR_FAILED, scram_set_user(cli, "al\0ice", 6));
    CuAssertTrue(tc, NULL == cli->user);
    CuAssertIntEquals(tc, ERR_OK, scram_set_user(cli, "alice", 5));
    CuAssertStrEquals(tc, "alice", cli->user);
    CuAssertIntEquals(tc, ERR_FAILED, scram_set_user(cli, "al\0ice", 6));
    CuAssertStrEquals(tc, "alice", cli->user);
    scram_free(cli);

    char msg[] = "n,,n=admin,r=Ym9ndXNub25jZQ==";
    size_t mlen = sizeof(msg) - 1;
    scram_ctx *ok = scram_init("SCRAM-SHA-256", 0);
    CuAssertPtrNotNull(tc, ok);
    CuAssertIntEquals(tc, ERR_OK, scram_parse_first_message(ok, msg, mlen));
    CuAssertStrEquals(tc, "admin", scram_get_user(ok));
    scram_free(ok);

    msg[7] = '\0';
    scram_ctx *bad = scram_init("SCRAM-SHA-256", 0);
    CuAssertPtrNotNull(tc, bad);
    CuAssertTrue(tc, ERR_OK != scram_parse_first_message(bad, msg, mlen));
    CuAssertTrue(tc, NULL == scram_get_user(bad));
    scram_free(bad);
}

/* =======================================================================
 * cipher —— AES / DES 加解密往返验证
 * ======================================================================= */
static void test_cipher(CuTest *tc) {
    const char *key16 = "0123456789abcdef"; /* AES-128 密钥（16 字节）*/
    const char *iv16 = "abcdef0123456789"; /* CBC/CFB/OFB/CTR IV */
    const char *plain = "Hello, Cipher!!!"; /* 整块明文（16 字节）*/
    const char *plain2 = "short";/* 非整块明文（5 字节）*/
    char enc_buf[64], dec_buf[64];
    size_t enc_len, dec_len;
    cipher_ctx enc, dec;

    /* ── AES-128 ECB + PKCS7 往返 ── */
    cipher_init(&enc, AES, ECB, key16, 16, 128, 1);
    cipher_padding(&enc, PKCS57);
    cipher_init(&dec, AES, ECB, key16, 16, 128, 0);
    cipher_padding(&dec, PKCS57);

    CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&enc, plain, 16, enc_buf, &enc_len));
    CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&dec, enc_buf, enc_len, dec_buf, &dec_len));
    CuAssertTrue(tc, 16 == (int)dec_len);
    CuAssertTrue(tc, 0 == memcmp(plain, dec_buf, 16));

    /* 非整块数据（5 字节），填充后可正确还原 */
    CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&enc, plain2, strlen(plain2), enc_buf, &enc_len));
    CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&dec, enc_buf, enc_len, dec_buf, &dec_len));
    CuAssertTrue(tc, (int)strlen(plain2) == (int)dec_len);
    CuAssertTrue(tc, 0 == memcmp(plain2, dec_buf, dec_len));

    /* ── AES-128 CBC + PKCS7 往返 ── */
    cipher_init(&enc, AES, CBC, key16, 16, 128, 1);
    cipher_padding(&enc, PKCS57);
    cipher_iv(&enc, iv16, 16);
    cipher_init(&dec, AES, CBC, key16, 16, 128, 0);
    cipher_padding(&dec, PKCS57);
    cipher_iv(&dec, iv16, 16);

    CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&enc, plain, 16, enc_buf, &enc_len));
    CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&dec, enc_buf, enc_len, dec_buf, &dec_len));
    CuAssertTrue(tc, 16 == (int)dec_len);
    CuAssertTrue(tc, 0 == memcmp(plain, dec_buf, 16));

    /* ECB 与 CBC 密文不同（CBC 受 IV 影响） */
    char ecb_enc[64], cbc_enc[64];
    cipher_init(&enc, AES, ECB, key16, 16, 128, 1);
    cipher_padding(&enc, PKCS57);
    CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&enc, plain, 16, ecb_enc, &enc_len));

    cipher_init(&enc, AES, CBC, key16, 16, 128, 1);
    cipher_padding(&enc, PKCS57);
    cipher_iv(&enc, iv16, 16);
    CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&enc, plain, 16, cbc_enc, &enc_len));
    CuAssertTrue(tc, 0 != memcmp(ecb_enc, cbc_enc, 16));

    /* ── DES ECB + PKCS7 往返（DES 密钥 8 字节，分组 8 字节）── */
    const char *des_key = "8bytekey";
    cipher_init(&enc, DES, ECB, des_key, 8, 0, 1);
    cipher_padding(&enc, PKCS57);
    cipher_init(&dec, DES, ECB, des_key, 8, 0, 0);
    cipher_padding(&dec, PKCS57);

    CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&enc, plain2, strlen(plain2), enc_buf, &enc_len));
    CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&dec, enc_buf, enc_len, dec_buf, &dec_len));
    CuAssertTrue(tc, (int)strlen(plain2) == (int)dec_len);
    CuAssertTrue(tc, 0 == memcmp(plain2, dec_buf, dec_len));

    /* cipher_size 返回当前引擎分组长度 */
    CuAssertTrue(tc, 8 == (int)cipher_size(&dec)); /* DES 分组 8 字节 */
    cipher_free(&enc);
    cipher_free(&dec);
}

/* 解密成功后剥离的 padding 字节应被清零(与失败路径 secure_zero 卫生一致) */
static void test_cipher_padding_zeroed(CuTest *tc) {
    const char *key16 = "0123456789abcdef";
    const char *plain = "Hello, Cipher!!!"; /* 16 字节整块 → PKCS7 补满一整块 */
    char enc_buf[64], dec_buf[64];
    cipher_ctx enc, dec;

    cipher_init(&enc, AES, ECB, key16, 16, 128, 1);
    cipher_padding(&enc, PKCS57);
    cipher_init(&dec, AES, ECB, key16, 16, 128, 0);
    cipher_padding(&dec, PKCS57);

    size_t enc_len;
    CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&enc, plain, 16, enc_buf, &enc_len));
    CuAssertTrue(tc, 32 == (int)enc_len);/* 16 数据 + 16 填充块 */

    memset(dec_buf, 0x5a, sizeof(dec_buf));
    size_t dec_len;
    CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&dec, enc_buf, enc_len, dec_buf, &dec_len));
    CuAssertTrue(tc, 16 == (int)dec_len);
    CuAssertTrue(tc, 0 == memcmp(plain, dec_buf, 16));
    /* 剥离的 16 字节 padding(原值 0x10)修复后应已清零 */
    char zero[16] = { 0 };
    CuAssertTrue(tc, 0 == memcmp(dec_buf + dec_len, zero, enc_len - dec_len));

    cipher_free(&enc);
    cipher_free(&dec);
}

/* 解密遇非法 padding 应拒绝:返回 0 且 output 清零(常数时间校验路径,与成功剥离对称) */
static void test_cipher_decrypt_bad_padding(CuTest *tc) {
    const char *key16 = "0123456789abcdef";
    const char *plain = "Hello, Cipher!!!"; /* 16 字节,末字节 '!'=0x21=33 */
    char enc_buf[64], dec_buf[64];
    cipher_ctx enc, dec;
    size_t enc_len, dec_len;
    char zero[16] = { 0 };

    cipher_init(&enc, AES, ECB, key16, 16, 128, 1);
    cipher_padding(&enc, PKCS57);
    cipher_init(&dec, AES, ECB, key16, 16, 128, 0);
    cipher_padding(&dec, PKCS57);

    CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&enc, plain, 16, enc_buf, &enc_len));
    CuAssertTrue(tc, 32 == (int)enc_len); /* 16 数据块 + 16 PKCS7 整填充块 */

    /* 只取首个密文块(16B 数据块)解密:还原 "Hello, Cipher!!!",
     * 末字节当 pad=0x21=33 > 块长 16 → padding 非法 → 拒绝,返回 0 且 output 清零 */
    memset(dec_buf, 0x5a, sizeof(dec_buf));
    CuAssertIntEquals(tc, ERR_FAILED, cipher_dofinal(&dec, enc_buf, 16, dec_buf, &dec_len));
    CuAssertTrue(tc, 0 == (int)dec_len);
    CuAssertTrue(tc, 0 == memcmp(dec_buf, zero, 16));

    cipher_free(&enc);
    cipher_free(&dec);
}

/* =======================================================================
 * HMAC —— SHA-1 / SHA-512 已知测试向量
 * ======================================================================= */
static void test_hmac_variants(CuTest *tc) {
    char hash[DG_BLOCK_SIZE];
    char hex[HEX_ENSIZE(DG_BLOCK_SIZE)];
    hmac_ctx hm;
    size_t hlen;

    /* RFC 2202 Test Case 1：HMAC-SHA1
     * Key  = 0x0b × 20，Data = "Hi There"
     * HMAC = b617318655057264e28bc0b6fb378c8ef146be00 */
    char key20[20];
    memset(key20, 0x0b, sizeof(key20));
    hmac_init(&hm, DG_SHA1, key20, sizeof(key20));
    hmac_update(&hm, "Hi There", 8);
    hlen = hmac_final(&hm, hash);
    tohex(hash, hlen, hex, 1);
    CuAssertTrue(tc, 20 == (int)hlen);
    CuAssertStrEquals(tc, "b617318655057264e28bc0b6fb378c8ef146be00", hex);

    /* hmac_size 返回正确摘要长度 */
    CuAssertTrue(tc, 20 == (int)hmac_size(&hm));

    /* RFC 4231 Test Case 1：HMAC-SHA512
     * Key  = 0x0b × 20，Data = "Hi There"
     * HMAC = 87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde
     *        daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854 */
    hmac_init(&hm, DG_SHA512, key20, sizeof(key20));
    hmac_update(&hm, "Hi There", 8);
    hlen = hmac_final(&hm, hash);
    tohex(hash, hlen, hex, 1);
    CuAssertTrue(tc, 64 == (int)hlen);
    CuAssertStrEquals(tc,
        "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
        "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854",
        hex);

    /* reset 后 SHA512 结果一致 */
    hmac_reset(&hm);
    hmac_update(&hm, "Hi There", 8);
    char hash2[DG_BLOCK_SIZE];
    hmac_final(&hm, hash2);
    CuAssertTrue(tc, 0 == memcmp(hash, hash2, hlen));
}

/* =======================================================================
 * MD2 —— RFC 1319 已知向量
 * ======================================================================= */
static void test_md2(CuTest *tc) {
    md2_ctx ctx;
    char hash[MD2_BLOCK_SIZE];
    char hex[HEX_ENSIZE(MD2_BLOCK_SIZE)];

    /* RFC 1319 Appendix A.5 测试向量 */
    /* MD2("") = 8350e5a3e24c153df2275c9f80692773 */
    md2_init(&ctx);
    md2_update(&ctx, "", 0);
    md2_final(&ctx, hash);
    tohex(hash, MD2_BLOCK_SIZE, hex, 1);
    CuAssertStrEquals(tc, "8350e5a3e24c153df2275c9f80692773", hex);

    /* MD2("a") = 32ec01ec4a6dac72c0ab96fb34c0b5d1 */
    md2_init(&ctx);
    md2_update(&ctx, "a", 1);
    md2_final(&ctx, hash);
    tohex(hash, MD2_BLOCK_SIZE, hex, 1);
    CuAssertStrEquals(tc, "32ec01ec4a6dac72c0ab96fb34c0b5d1", hex);

    /* MD2("abc") = da853b0d3f88d99b30283a69e6ded6bb */
    md2_init(&ctx);
    md2_update(&ctx, "abc", 3);
    md2_final(&ctx, hash);
    tohex(hash, MD2_BLOCK_SIZE, hex, 1);
    CuAssertStrEquals(tc, "da853b0d3f88d99b30283a69e6ded6bb", hex);

    /* MD2("message digest") = ab4f496bfb2a530b219ff33031fe06b0 */
    md2_init(&ctx);
    md2_update(&ctx, "message digest", 14);
    md2_final(&ctx, hash);
    tohex(hash, MD2_BLOCK_SIZE, hex, 1);
    CuAssertStrEquals(tc, "ab4f496bfb2a530b219ff33031fe06b0", hex);

    /* A.5 余下三条，长度 26 / 62 / 80 —— 上面四条最长 14 字节，全在 MD2 的 16 字节分组之内，
       md2.c 的跨块补齐与 bulk 循环一次都没执行过 */
    struct { const char *in; const char *expect; } big[] = {
        { "abcdefghijklmnopqrstuvwxyz",                                     "4e8ddff3650292ab5a4108c3aa47940b" },
        { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "da33def2a42df13975352846c30338cd" },
        { "12345678901234567890123456789012345678901234567890"
          "123456789012345678901234567890",                                 "d5976f79d83d3a0dc9806c3c66f3efd8" },
    };
    for (size_t i = 0; i < sizeof(big) / sizeof(big[0]); i++) {
        md2_init(&ctx);
        md2_update(&ctx, big[i].in, strlen(big[i].in));
        md2_final(&ctx, hash);
        tohex(hash, MD2_BLOCK_SIZE, hex, 1);
        CuAssertStrEquals(tc, big[i].expect, hex);
    }

    /* 分段 update 与整体 update 结果相同 */
    char h1[MD2_BLOCK_SIZE], h2[MD2_BLOCK_SIZE];
    md2_init(&ctx);
    md2_update(&ctx, "abcdef", 6);
    md2_final(&ctx, h1);

    md2_init(&ctx);
    md2_update(&ctx, "abc", 3);
    md2_update(&ctx, "def", 3);
    md2_final(&ctx, h2);
    CuAssertTrue(tc, 0 == memcmp(h1, h2, MD2_BLOCK_SIZE));
}

/* =======================================================================
 * MD4 —— RFC 1320 已知向量
 * ======================================================================= */
static void test_md4(CuTest *tc) {
    md4_ctx ctx;
    char hash[MD4_BLOCK_SIZE];
    char hex[HEX_ENSIZE(MD4_BLOCK_SIZE)];

    /* MD4("") = 31d6cfe0d16ae931b73c59d7e0c089c0 */
    md4_init(&ctx);
    md4_update(&ctx, "", 0);
    md4_final(&ctx, hash);
    tohex(hash, MD4_BLOCK_SIZE, hex, 1);
    CuAssertStrEquals(tc, "31d6cfe0d16ae931b73c59d7e0c089c0", hex);

    /* MD4("a") = bde52cb31de33e46245e05fbdbd6fb24 */
    md4_init(&ctx);
    md4_update(&ctx, "a", 1);
    md4_final(&ctx, hash);
    tohex(hash, MD4_BLOCK_SIZE, hex, 1);
    CuAssertStrEquals(tc, "bde52cb31de33e46245e05fbdbd6fb24", hex);

    /* MD4("abc") = a448017aaf21d8525fc10ae87aa6729d */
    md4_init(&ctx);
    md4_update(&ctx, "abc", 3);
    md4_final(&ctx, hash);
    tohex(hash, MD4_BLOCK_SIZE, hex, 1);
    CuAssertStrEquals(tc, "a448017aaf21d8525fc10ae87aa6729d", hex);

    /* MD4("message digest") = d9130a8164549fe818874806e1c7014b */
    md4_init(&ctx);
    md4_update(&ctx, "message digest", 14);
    md4_final(&ctx, hash);
    tohex(hash, MD4_BLOCK_SIZE, hex, 1);
    CuAssertStrEquals(tc, "d9130a8164549fe818874806e1c7014b", hex);

    /* MD4("abcdefghijklmnopqrstuvwxyz") = d79e1c308aa5bbcdeea8ed63df412da9 */
    md4_init(&ctx);
    md4_update(&ctx, "abcdefghijklmnopqrstuvwxyz", 26);
    md4_final(&ctx, hash);
    tohex(hash, MD4_BLOCK_SIZE, hex, 1);
    CuAssertStrEquals(tc, "d79e1c308aa5bbcdeea8ed63df412da9", hex);

    /* A.5 余下两条，62 / 80 字节。上面五条最长 26 字节，全在 64 字节分组之内，
       位计数器与 bulk 循环算错都看不出来；80 字节那条是唯一跨分组的官方向量 */
    struct { const char *in; const char *expect; } big[] = {
        { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", "043f8582f241db351ce627e153e7f0e4" },
        { "12345678901234567890123456789012345678901234567890"
          "123456789012345678901234567890",                                 "e33b4ddc9c38f2199c3e7b164fcc0536" },
    };
    for (size_t i = 0; i < sizeof(big) / sizeof(big[0]); i++) {
        md4_init(&ctx);
        md4_update(&ctx, big[i].in, strlen(big[i].in));
        md4_final(&ctx, hash);
        tohex(hash, MD4_BLOCK_SIZE, hex, 1);
        CuAssertStrEquals(tc, big[i].expect, hex);
    }

    /* 分段 update 与整体 update 结果相同 */
    char h1[MD4_BLOCK_SIZE], h2[MD4_BLOCK_SIZE];
    md4_init(&ctx);
    md4_update(&ctx, "abcdefghij", 10);
    md4_final(&ctx, h1);

    md4_init(&ctx);
    md4_update(&ctx, "abcde", 5);
    md4_update(&ctx, "fghij", 5);
    md4_final(&ctx, h2);
    CuAssertTrue(tc, 0 == memcmp(h1, h2, MD4_BLOCK_SIZE));
}

// md4_update 从"分支判定用 (uint32_t)lens、末尾 memcpy 用完整 size_t"改为全程 size_t 之后的
// 等价性守卫。真正的溢出要单次喂 >=4GiB 才触发，测不起；这里钉住改写没有动缓冲推进逻辑：
// 各种切分（不足一块 / 正好一块 / 跨块 / 跨多块 / 尾块不齐）都要与一次性喂入同摘要。
// 顺带钉住 lens==0 是空操作——改前它落到 memcpy(dst, p, 0)，data 传 NULL 时属 UB
static void test_md4_update_chunked(CuTest *tc) {
    uint8_t buf[200];
    size_t chunks[] = { 1, 7, 63, 64, 65, 100, 199, 200 };
    char whole[MD4_BLOCK_SIZE], part[MD4_BLOCK_SIZE];
    md4_ctx ctx;
    size_t i, off, step;

    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(i * 7 + 1);
    }
    md4_init(&ctx);
    md4_update(&ctx, buf, sizeof(buf));
    md4_final(&ctx, whole);

    for (i = 0; i < ARRAY_SIZE(chunks); i++) {
        md4_init(&ctx);
        off = 0;
        while (off < sizeof(buf)) {
            step = chunks[i];
            if (off + step > sizeof(buf)) {
                step = sizeof(buf) - off;
            }
            md4_update(&ctx, buf + off, step);
            off += step;
        }
        md4_final(&ctx, part);
        CuAssertTrue(tc, 0 == memcmp(whole, part, MD4_BLOCK_SIZE));
    }

    // 零长 update 穿插在任意位置都不该改变结果
    md4_init(&ctx);
    md4_update(&ctx, NULL, 0);
    md4_update(&ctx, buf, 64);
    md4_update(&ctx, buf, 0);
    md4_update(&ctx, buf + 64, sizeof(buf) - 64);
    md4_update(&ctx, NULL, 0);
    md4_final(&ctx, part);
    CuAssertTrue(tc, 0 == memcmp(whole, part, MD4_BLOCK_SIZE));
}

// md5 直调 RFC 1321 标准测试向量集
static void test_md5_nist(CuTest *tc) {
    md5_ctx ctx;
    char hash[MD5_BLOCK_SIZE];
    char hex[HEX_ENSIZE(MD5_BLOCK_SIZE)];
    struct { const char *in; const char *expect; } cases[] = {
        { "",                                                                "d41d8cd98f00b204e9800998ecf8427e" },
        { "a",                                                               "0cc175b9c0f1b6a831c399e269772661" },
        { "abc",                                                             "900150983cd24fb0d6963f7d28e17f72" },
        { "message digest",                                                  "f96b697d7cb7938d525a2f31aaf161d0" },
        { "abcdefghijklmnopqrstuvwxyz",                                      "c3fcd3d76192e4007dfb496cca67e13b" },
        { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",  "d174ab98d277d9f5a5611c2c9f419d9f" },
        /* A.5 最后一条，80 字节 —— 唯一一条跨过 64 字节分组的官方向量。
           上面五条都 < 64，位计数器算错（bitlen += 512 写成 += 256）它们全看不出来 */
        { "12345678901234567890123456789012345678901234567890"
          "123456789012345678901234567890",                                  "57edf4a22be3c955ac49da2e2107b67a" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        md5_init(&ctx);
        md5_update(&ctx, cases[i].in, strlen(cases[i].in));
        md5_final(&ctx, hash);
        tohex(hash, MD5_BLOCK_SIZE, hex, 1);
        CuAssertStrEquals(tc, cases[i].expect, hex);
    }
    // 跨 block（>64 字节）正确性：800 个 'a' 触发多 block + final padding
    char buf[800];
    memset(buf, 'a', sizeof(buf));
    md5_init(&ctx);
    md5_update(&ctx, buf, sizeof(buf));
    md5_final(&ctx, hash);
    tohex(hash, MD5_BLOCK_SIZE, hex, 1);
    char hash2[MD5_BLOCK_SIZE];
    char hex2[HEX_ENSIZE(MD5_BLOCK_SIZE)];
    // 分段 update 与整体 update 结果一致
    md5_init(&ctx);
    md5_update(&ctx, buf, 400);
    md5_update(&ctx, buf + 400, 400);
    md5_final(&ctx, hash2);
    tohex(hash2, MD5_BLOCK_SIZE, hex2, 1);
    CuAssertStrEquals(tc, hex, hex2);
}

// sha1 直调 FIPS 180-1 / RFC 3174 标准测试向量集
static void test_sha1_nist(CuTest *tc) {
    sha1_ctx ctx;
    char hash[SHA1_BLOCK_SIZE];
    char hex[HEX_ENSIZE(SHA1_BLOCK_SIZE)];
    struct { const char *in; const char *expect; } cases[] = {
        { "",     "da39a3ee5e6b4b0d3255bfef95601890afd80709" },
        { "abc",  "a9993e364706816aba3e25717850c26c9cd0d89d" },
        // FIPS 180-1 二段标准向量（56 字节，正好 padding 临界）
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        sha1_init(&ctx);
        sha1_update(&ctx, cases[i].in, strlen(cases[i].in));
        sha1_final(&ctx, hash);
        tohex(hash, SHA1_BLOCK_SIZE, hex, 1);
        CuAssertStrEquals(tc, cases[i].expect, hex);
    }
    // 1 百万个 'a'：FIPS 180-1 长输入向量 34aa973cd4c4daa4f61eeb2bdbad27316534016f
    sha1_init(&ctx);
    char chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    for (int i = 0; i < 1000; i++) {
        sha1_update(&ctx, chunk, sizeof(chunk));
    }
    sha1_final(&ctx, hash);
    tohex(hash, SHA1_BLOCK_SIZE, hex, 1);
    CuAssertStrEquals(tc, "34aa973cd4c4daa4f61eeb2bdbad27316534016f", hex);
}

// sha256 直调 FIPS 180-2 标准测试向量集
static void test_sha256_nist(CuTest *tc) {
    sha256_ctx ctx;
    char hash[SHA256_BLOCK_SIZE];
    char hex[HEX_ENSIZE(SHA256_BLOCK_SIZE)];
    struct { const char *in; const char *expect; } cases[] = {
        { "",     "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc",  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        // FIPS 180-2 二段向量（56 字节，临界 padding）
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        sha256_init(&ctx);
        sha256_update(&ctx, cases[i].in, strlen(cases[i].in));
        sha256_final(&ctx, hash);
        tohex(hash, SHA256_BLOCK_SIZE, hex, 1);
        CuAssertStrEquals(tc, cases[i].expect, hex);
    }
    // 分段 update（单字节流） vs 整体 update 等价
    const char *msg = "The quick brown fox jumps over the lazy dog";
    size_t mlen = strlen(msg);
    sha256_init(&ctx);
    sha256_update(&ctx, msg, mlen);
    sha256_final(&ctx, hash);
    char hex_whole[HEX_ENSIZE(SHA256_BLOCK_SIZE)];
    tohex(hash, SHA256_BLOCK_SIZE, hex_whole, 1);
    sha256_ctx ctx2;
    sha256_init(&ctx2);
    for (size_t i = 0; i < mlen; i++) {
        sha256_update(&ctx2, msg + i, 1);
    }
    sha256_final(&ctx2, hash);
    char hex_byte[HEX_ENSIZE(SHA256_BLOCK_SIZE)];
    tohex(hash, SHA256_BLOCK_SIZE, hex_byte, 1);
    CuAssertStrEquals(tc, hex_whole, hex_byte);
    // 已知值：SHA256("The quick brown fox jumps over the lazy dog")
    CuAssertStrEquals(tc, "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592", hex_whole);
}

// sha512 直调 FIPS 180-2 标准测试向量集
static void test_sha512_nist(CuTest *tc) {
    sha512_ctx ctx;
    char hash[SHA512_BLOCK_SIZE];
    char hex[HEX_ENSIZE(SHA512_BLOCK_SIZE)];
    sha512_init(&ctx);
    sha512_update(&ctx, "", 0);
    sha512_final(&ctx, hash);
    tohex(hash, SHA512_BLOCK_SIZE, hex, 1);
    CuAssertStrEquals(tc,
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
        hex);
    sha512_init(&ctx);
    sha512_update(&ctx, "abc", 3);
    sha512_final(&ctx, hash);
    tohex(hash, SHA512_BLOCK_SIZE, hex, 1);
    CuAssertStrEquals(tc,
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
        hex);
    // 缓冲里已有半块时喂 (NULL, 0)：绑定层的 lpub_check_buf 放行这种入参，摘要必须与不喂时一致。
    // 只有 Linux 的 glibc 构建 + sh mk.sh test asan debug 才会因 memcpy 的 __nonnull 变红，macOS 不报
    sha512_init(&ctx);
    sha512_update(&ctx, "abc", 3);
    sha512_update(&ctx, NULL, 0);
    sha512_final(&ctx, hash);
    tohex(hash, SHA512_BLOCK_SIZE, hex, 1);
    CuAssertStrEquals(tc,
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
        hex);
    // FIPS 180-2 二段长输入（112 字节，临界 128-byte block + padding）
    const char *two_block =
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
        "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    sha512_init(&ctx);
    sha512_update(&ctx, two_block, strlen(two_block));
    sha512_final(&ctx, hash);
    tohex(hash, SHA512_BLOCK_SIZE, hex, 1);
    CuAssertStrEquals(tc,
        "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
        "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909",
        hex);
    // 1 百万个 'a'：FIPS 180-2 长输入向量。分 1000 段各 1000 字节喂，而 1000 不是 128 的
    // 整数倍，所以每段都跨内部缓冲边界——上面三条都是一次喂完（长度全 < 128），
    // sha512.c 的 `p += freespace` 那条跨块续接路径一次都没执行过，改成 `p += lens` 也照过
    sha512_init(&ctx);
    char chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    for (int i = 0; i < 1000; i++) {
        sha512_update(&ctx, chunk, sizeof(chunk));
    }
    sha512_final(&ctx, hash);
    tohex(hash, SHA512_BLOCK_SIZE, hex, 1);
    CuAssertStrEquals(tc,
        "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
        "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b",
        hex);
}

/* 多块输入：1000 字节、逐字节不同（期望值由 Python hashlib 与按 RFC 1319 独立实现的 MD2 算出）。
 * 一次喂完走各算法的多块变换；逐字节喂只走单块路径；先喂 3 字节再喂余下的，走"先补满缓冲、再整块批量、
 * 余下进缓冲"的衔接。三种喂法都要等于外部向量。原先的长输入都是同一个字节，变换里漏了指针前移
 * （每块都在算第一块）也照过，sha256 的多块路径则一次都没走到 */
static void test_digest_multiblock(CuTest *tc) {
    static const struct {
        digest_type t;
        const char *hex;
    } vec[] = {
        { DG_MD2,    "f7292ceca8085c9ee6d9177305eee9d4" },
        { DG_MD4,    "26634622b1025dcfdef6c1b6d0e3f8fc" },
        { DG_MD5,    "0b8ae90ded6089334e353eb2669ab5e6" },
        { DG_SHA1,   "425b5f2d2d344f4f6467cda9065cdc840619dc2d" },
        { DG_SHA256, "533b698850849b7908b20a22658f639c0b2a476f1791f85f50188287c31a9aba" },
        { DG_SHA512, "7881dc60b1a8061810f37f35e9b90cd7725a42f8bbdd7eb516dba2563b4b1d89"
                     "a2f92967fb721d93a72df061f52a038b6e62473161cd132b3390101909c58b2e" },
    };
    uint8_t buf[1000];
    char out[DG_BLOCK_SIZE];
    char hex[HEX_ENSIZE(DG_BLOCK_SIZE)];
    digest_ctx d;
    size_t i, j, n;
    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(i * 131 + 7);
    }
    for (i = 0; i < ARRAY_SIZE(vec); i++) {
        digest_init(&d, vec[i].t);
        digest_update(&d, buf, sizeof(buf));
        n = digest_final(&d, out);
        tohex(out, n, hex, 1);
        CuAssertStrEquals(tc, vec[i].hex, hex);
        digest_free(&d);

        digest_init(&d, vec[i].t);
        for (j = 0; j < sizeof(buf); j++) {
            digest_update(&d, buf + j, 1);
        }
        n = digest_final(&d, out);
        tohex(out, n, hex, 1);
        CuAssertStrEquals(tc, vec[i].hex, hex);
        digest_free(&d);

        digest_init(&d, vec[i].t);
        digest_update(&d, buf, 3);
        digest_update(&d, buf + 3, sizeof(buf) - 3);
        n = digest_final(&d, out);
        tohex(out, n, hex, 1);
        CuAssertStrEquals(tc, vec[i].hex, hex);
        digest_free(&d);
    }
}

// 将 hex 字符串转为字节数组，长度必须是偶数
// 逐字节解一条十六进制测试向量。写错的向量当场 abort，别静默解出另一组字节
static void _hex_to_bytes(const char *hex, uint8_t *out, size_t outlen) {
    int32_t v1, v2;
    size_t i;
    for (i = 0; i < outlen; i++) {
        v1 = fromhex(hex[i * 2]);
        v2 = fromhex(hex[i * 2 + 1]);
        ASSERTAB(ERR_FAILED != v1 && ERR_FAILED != v2, "invalid hex vector");
        out[i] = (uint8_t)((v1 << 4) | v2);
    }
}

/* cipher 层各模式的 NIST SP 800-38A 已知答案。走 cipher_block（下方 test_aes_direct
 * 走的是 aes_crypt 直调，两条路都要钉）。每个模式跑**两块**：只跑第一块的话
 * CBC 的链接与 CTR 的计数器进位都执行不到（_cipher_inc_iv 改成小端进位时正是第二块起才不对）。
 * key / plaintext 与 test_aes_direct 同源 */
static void _cipher_kat(CuTest *tc, cipher_model model, const char *ivhex,
    const char *c1hex, const char *c2hex) {
    const char *keyhex = "2b7e151628aed2a6abf7158809cf4f3c";
    const char *p1hex = "6bc1bee22e409f96e93d7e117393172a";
    const char *p2hex = "ae2d8a571e03ac9c9eb76fac45af8e51";
    uint8_t key[16], iv[16], p1[16], p2[16];
    char hex[HEX_ENSIZE(AES_BLOCK_SIZE)];
    cipher_ctx enc, dec;
    void *out;
    size_t olen;
    _hex_to_bytes(keyhex, key, 16);
    _hex_to_bytes(ivhex, iv, 16);
    _hex_to_bytes(p1hex, p1, 16);
    _hex_to_bytes(p2hex, p2, 16);

    cipher_init(&enc, AES, model, (const char *)key, 16, 128, 1);
    if (ECB != model) {
        cipher_iv(&enc, (const char *)iv, 16);
    }
    out = cipher_block(&enc, p1, 16, &olen);
    CuAssertPtrNotNull(tc, out);
    CuAssertTrue(tc, 16 == olen);
    tohex(out, 16, hex, 1);
    CuAssertStrEquals(tc, c1hex, hex);
    out = cipher_block(&enc, p2, 16, &olen);
    CuAssertPtrNotNull(tc, out);
    tohex(out, 16, hex, 1);
    CuAssertStrEquals(tc, c2hex, hex);

    /* 反向：同样两块密文必须还原出原明文（CTR/OFB 的解密与加密同路，这一步只作对照） */
    uint8_t c1[16], c2[16];
    _hex_to_bytes(c1hex, c1, 16);
    _hex_to_bytes(c2hex, c2, 16);
    cipher_init(&dec, AES, model, (const char *)key, 16, 128, 0);
    if (ECB != model) {
        cipher_iv(&dec, (const char *)iv, 16);
    }
    out = cipher_block(&dec, c1, 16, &olen);
    CuAssertPtrNotNull(tc, out);
    CuAssertTrue(tc, 0 == memcmp(out, p1, 16));
    out = cipher_block(&dec, c2, 16, &olen);
    CuAssertPtrNotNull(tc, out);
    CuAssertTrue(tc, 0 == memcmp(out, p2, 16));
}
static void test_cipher_nist_modes(CuTest *tc) {
    /* F.1.1 ECB-AES128.Encrypt（ECB 无 IV，helper 跳过 cipher_iv）。
       cipher 层的 ECB 原来只有 round-trip，把加解密都换成恒等映射也全绿 */
    _cipher_kat(tc, ECB, "00000000000000000000000000000000",
        "3ad77bb40d7a3660a89ecaf32466ef97", "f5d3d58503b9699de785895a96fdbaaf");
    /* F.2.1 CBC-AES128.Encrypt */
    _cipher_kat(tc, CBC, "000102030405060708090a0b0c0d0e0f",
        "7649abac8119b246cee98e9b12e9197d", "5086cb9b507219ee95db113a917678b2");
    /* F.3.13 CFB128-AES128.Encrypt */
    _cipher_kat(tc, CFB, "000102030405060708090a0b0c0d0e0f",
        "3b3fd92eb72dad20333449f8e83cfb4a", "c8a64537a0b3a93fcde3cdad9f1ce58b");
    /* F.4.1 OFB-AES128.Encrypt（首块与 CFB 相同，都是 E(IV) xor P1；第二块才分道） */
    _cipher_kat(tc, OFB, "000102030405060708090a0b0c0d0e0f",
        "3b3fd92eb72dad20333449f8e83cfb4a", "7789508d16918f03f53c52dac54ed825");
    /* F.5.1 CTR-AES128.Encrypt，初始计数器 f0f1..ff：第二块要靠末字节 ff 进位到 fe */
    _cipher_kat(tc, CTR, "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
        "874d6191b620e3261bef6864990db6ce", "9806f66b7970fdff8617187bb9fffdff");
}

/* cipher_dofinal 的 AES 整分组批量路径：SP 800-38A 的四块明文一次喂完（NoPadding，64 字节全走批量），
 * 加解密都要对上已知答案，异址与原地（output 与 data 同址）各跑一遍。上面的 _cipher_kat 走 cipher_block，
 * 批量路径原来只有往返测试，OFB 这种加解密同路的模式算错了往返照过。四块密文由 pycryptodome 算出，
 * 前两块与上面的 NIST 表一致 */
static void test_cipher_dofinal_batch_kat(CuTest *tc) {
    static const struct {
        cipher_model m;
        const char *iv;
        const char *ct;
    } vec[] = {
        { ECB, NULL,
          "3ad77bb40d7a3660a89ecaf32466ef97f5d3d58503b9699de785895a96fdbaaf"
          "43b1cd7f598ece23881b00e3ed0306887b0c785e27e8ad3f8223207104725dd4" },
        { CBC, "000102030405060708090a0b0c0d0e0f",
          "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
          "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7" },
        { CFB, "000102030405060708090a0b0c0d0e0f",
          "3b3fd92eb72dad20333449f8e83cfb4ac8a64537a0b3a93fcde3cdad9f1ce58b"
          "26751f67a3cbb140b1808cf187a4f4dfc04b05357c5d1c0eeac4c66f9ff7f2e6" },
        { OFB, "000102030405060708090a0b0c0d0e0f",
          "3b3fd92eb72dad20333449f8e83cfb4a7789508d16918f03f53c52dac54ed825"
          "9740051e9c5fecf64344f7a82260edcc304c6528f659c77866a510d9c1d6ae5e" },
        { CTR, "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
          "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
          "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee" },
    };
    const char *keyhex = "2b7e151628aed2a6abf7158809cf4f3c";
    const char *pthex = "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                        "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710";
    uint8_t key[16], iv[16], pt[64], ct[64], buf[64];
    const uint8_t *from, *want;
    cipher_ctx c;
    size_t i, olen;
    int32_t enc;
    _hex_to_bytes(keyhex, key, 16);
    _hex_to_bytes(pthex, pt, 64);
    for (i = 0; i < ARRAY_SIZE(vec); i++) {
        _hex_to_bytes(vec[i].ct, ct, 64);
        for (enc = 1; enc >= 0; enc--) {
            from = enc ? pt : ct;
            want = enc ? ct : pt;
            cipher_init(&c, AES, vec[i].m, (const char *)key, 16, 128, enc);
            cipher_padding(&c, NoPadding);
            if (NULL != vec[i].iv) {
                _hex_to_bytes(vec[i].iv, iv, 16);
                cipher_iv(&c, (const char *)iv, 16);
            }
            CuAssertTrue(tc, ERR_OK == cipher_dofinal(&c, from, 64, (char *)buf, &olen));
            CuAssertTrue(tc, 64 == olen);
            CuAssertTrue(tc, 0 == memcmp(buf, want, 64));
            memcpy(buf, from, 64);
            CuAssertTrue(tc, ERR_OK == cipher_dofinal(&c, buf, 64, (char *)buf, &olen));
            CuAssertTrue(tc, 64 == olen);
            CuAssertTrue(tc, 0 == memcmp(buf, want, 64));
            cipher_free(&c);
        }
    }
}

// aes_init / aes_crypt 直调，NIST FIPS-197 Appendix A/B 标准向量 + 128/192/256 keybits
static void test_aes_direct(CuTest *tc) {
    aes_ctx aes;
    char hex[HEX_ENSIZE(AES_BLOCK_SIZE)];

    // ── AES-128 ECB（NIST SP 800-38A F.1.1）──
    {
        uint8_t key[16], pt[16];
        _hex_to_bytes("2b7e151628aed2a6abf7158809cf4f3c", key, 16);
        _hex_to_bytes("6bc1bee22e409f96e93d7e117393172a", pt, 16);
        aes_init(&aes, (char *)key, 16, 128, 1);
        char *ct = aes_crypt(&aes, pt);
        tohex(ct, AES_BLOCK_SIZE, hex, 1);
        CuAssertStrEquals(tc, "3ad77bb40d7a3660a89ecaf32466ef97", hex);
        // 解密回明文
        aes_init(&aes, (char *)key, 16, 128, 0);
        char *pt2 = aes_crypt(&aes, ct);
        CuAssertTrue(tc, 0 == memcmp(pt2, pt, 16));
    }

    // ── AES-192 ECB（NIST SP 800-38A F.1.3）──
    {
        uint8_t key[24], pt[16];
        _hex_to_bytes("8e73b0f7da0e6452c810f32b809079e562f8ead2522c6b7b", key, 24);
        _hex_to_bytes("6bc1bee22e409f96e93d7e117393172a", pt, 16);
        aes_init(&aes, (char *)key, 24, 192, 1);
        char *ct = aes_crypt(&aes, pt);
        tohex(ct, AES_BLOCK_SIZE, hex, 1);
        CuAssertStrEquals(tc, "bd334f1d6e45f25ff712a214571fa5cc", hex);
        aes_init(&aes, (char *)key, 24, 192, 0);
        char *pt2 = aes_crypt(&aes, ct);
        CuAssertTrue(tc, 0 == memcmp(pt2, pt, 16));
    }

    // ── AES-256 ECB（NIST SP 800-38A F.1.5）──
    {
        uint8_t key[32], pt[16];
        _hex_to_bytes("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", key, 32);
        _hex_to_bytes("6bc1bee22e409f96e93d7e117393172a", pt, 16);
        aes_init(&aes, (char *)key, 32, 256, 1);
        char *ct = aes_crypt(&aes, pt);
        tohex(ct, AES_BLOCK_SIZE, hex, 1);
        CuAssertStrEquals(tc, "f3eed1bdb5d2a03c064b5a7e3db181f8", hex);
        aes_init(&aes, (char *)key, 32, 256, 0);
        char *pt2 = aes_crypt(&aes, ct);
        CuAssertTrue(tc, 0 == memcmp(pt2, pt, 16));
    }

    // ── 短密钥自动零填充：klens < 16 时填充到 keybits 要求的长度 ──
    {
        uint8_t pt[16];
        memset(pt, 0xab, 16);
        const char *short_key = "abc";
        aes_init(&aes, short_key, 3, 128, 1);
        char *ct1 = aes_crypt(&aes, pt);
        char enc1[AES_BLOCK_SIZE];
        memcpy(enc1, ct1, AES_BLOCK_SIZE);
        // 同样的零填充密钥再来一次，结果应一致
        char padded_key[16];
        memset(padded_key, 0, 16);
        memcpy(padded_key, "abc", 3);
        aes_init(&aes, padded_key, 16, 128, 1);
        char *ct2 = aes_crypt(&aes, pt);
        CuAssertTrue(tc, 0 == memcmp(enc1, ct2, AES_BLOCK_SIZE));
    }
}

// des_init / des_crypt 直调 FIPS-46 标准向量 + 单 DES / 3DES round-trip
static void test_des_direct(CuTest *tc) {
    des_ctx des;

    // ── 单 DES（FIPS PUB 81 标准向量）──
    {
        uint8_t key[8], pt[8];
        _hex_to_bytes("0123456789ABCDEF", key, 8);
        _hex_to_bytes("4E6F772069732074", pt, 8); // "Now is t"
        des_init(&des, (char *)key, 8, 0, 1);
        char *ct = des_crypt(&des, pt);
        char hex[HEX_ENSIZE(DES_BLOCK_SIZE)];
        tohex(ct, DES_BLOCK_SIZE, hex, 1);
        CuAssertStrEquals(tc, "3fa40e8a984d4815", hex);
        // 解密回明文
        des_init(&des, (char *)key, 8, 0, 0);
        char *pt2 = des_crypt(&des, ct);
        CuAssertTrue(tc, 0 == memcmp(pt2, pt, 8));
    }

    // ── 3DES round-trip（自验证）──
    {
        uint8_t key[24], pt[8] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };
        _hex_to_bytes("0123456789ABCDEF23456789ABCDEF010123456789ABCDEF", key, 24);
        des_init(&des, (char *)key, 24, 1, 1);
        char ct_copy[DES_BLOCK_SIZE];
        memcpy(ct_copy, des_crypt(&des, pt), DES_BLOCK_SIZE);
        des_init(&des, (char *)key, 24, 1, 0);
        char *pt2 = des_crypt(&des, ct_copy);
        CuAssertTrue(tc, 0 == memcmp(pt2, pt, 8));
    }

    // ── 3DES 双密钥：16 字节密钥须等价于 24 字节的 K1|K2|K1（NIST SP 800-67 取法二）
    // 早先整体补零使 K3 全零，密文与任何合规实现都对不上
    {
        uint8_t key16[16], key24[24], pt[8] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };
        _hex_to_bytes("0123456789ABCDEF23456789ABCDEF01", key16, 16);
        memcpy(key24, key16, 16);
        memcpy(key24 + 16, key16, 8);
        des_init(&des, (char *)key16, 16, 1, 1);
        char ct16[DES_BLOCK_SIZE];
        memcpy(ct16, des_crypt(&des, pt), DES_BLOCK_SIZE);
        des_init(&des, (char *)key24, 24, 1, 1);
        CuAssertTrue(tc, 0 == memcmp(ct16, des_crypt(&des, pt), DES_BLOCK_SIZE));
        // 解密方向同样按 K3=K1 取，能还原明文
        des_init(&des, (char *)key16, 16, 1, 0);
        char *pt2 = des_crypt(&des, ct16);
        CuAssertTrue(tc, 0 == memcmp(pt2, pt, 8));
    }

    // ── 3DES 单密钥：8 字节按 K1=K2=K3 取，E-D-E 抵消后须等同单重 DES 的 FIPS 向量（取法三）──
    {
        uint8_t key[8], pt[8];
        _hex_to_bytes("0123456789ABCDEF", key, 8);
        _hex_to_bytes("4E6F772069732074", pt, 8);
        des_init(&des, (char *)key, 8, 1, 1);
        char hex[HEX_ENSIZE(DES_BLOCK_SIZE)];
        tohex(des_crypt(&des, pt), DES_BLOCK_SIZE, hex, 1);
        CuAssertStrEquals(tc, "3fa40e8a984d4815", hex);
    }

    // ── 3DES 非标准长度：10 字节补零到 16 后仍按 K3=K1 取 ──
    {
        uint8_t key24[24], pt[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        memset(key24, 0, sizeof(key24));
        memcpy(key24, "0123456789", 10);
        memcpy(key24 + 16, key24, 8);
        des_init(&des, "0123456789", 10, 1, 1);
        char ct_a[DES_BLOCK_SIZE];
        memcpy(ct_a, des_crypt(&des, pt), DES_BLOCK_SIZE);
        des_init(&des, (char *)key24, 24, 1, 1);
        CuAssertTrue(tc, 0 == memcmp(ct_a, des_crypt(&des, pt), DES_BLOCK_SIZE));
    }

    // ── 短密钥自动零填充：klens < 8 时填充 ──
    {
        uint8_t pt[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        des_init(&des, "x", 1, 0, 1);
        char ct_a[DES_BLOCK_SIZE];
        memcpy(ct_a, des_crypt(&des, pt), DES_BLOCK_SIZE);
        char padded_key[8];
        memset(padded_key, 0, 8);
        padded_key[0] = 'x';
        des_init(&des, padded_key, 8, 0, 1);
        char *ct_b = des_crypt(&des, pt);
        CuAssertTrue(tc, 0 == memcmp(ct_a, ct_b, DES_BLOCK_SIZE));
    }

    // ── 单 DES 更多已知答案（Grabbe 教程两条、Eric Young libdes ecb_data 表三条，均经 pycryptodome 复核）
    // 往返测试抓不住 S/P 合并表或 PC-2 表里的错项：Feistel 结构不管 F 是什么都能正确解回，
    // 子密钥错了也是加解密一起错。原来只有一条向量，只走得到表里很少的几项
    {
        static const struct { const char *k, *p, *c; } dv[] = {
            { "133457799BBCDFF1", "0123456789ABCDEF", "85e813540f0ab405" },
            { "0E329232EA6D0D73", "8787878787878787", "0000000000000000" },
            { "FEDCBA9876543210", "0123456789ABCDEF", "ed39d950fa74bcc4" },
            { "7CA110454A1A6E57", "01A1D6D039776742", "690f5b0d9a26939b" },
            { "0101010101010101", "0000000000000000", "8ca64de9c1b123a7" },
        };
        uint8_t key[8], pt[8], ct[8];
        char hex[HEX_ENSIZE(DES_BLOCK_SIZE)];
        size_t i;
        for (i = 0; i < ARRAY_SIZE(dv); i++) {
            _hex_to_bytes(dv[i].k, key, 8);
            _hex_to_bytes(dv[i].p, pt, 8);
            des_init(&des, (char *)key, 8, 0, 1);
            memcpy(ct, des_crypt(&des, pt), DES_BLOCK_SIZE);
            tohex((char *)ct, DES_BLOCK_SIZE, hex, 1);
            CuAssertStrEquals(tc, dv[i].c, hex);
            des_init(&des, (char *)key, 8, 0, 0);
            CuAssertTrue(tc, 0 == memcmp(des_crypt(&des, ct), pt, DES_BLOCK_SIZE));
        }
    }

    // ── 3DES 已知答案：三密钥取 NIST SP 800-67 的样例（三块 ECB），双密钥 K1|K2|K1 由 pycryptodome 算出
    {
        const char *pt3 = "The qufck brown fox jump";
        const char *want3 = "a826fd8ce53b855fcce21c8112256fe668d5c05dd9b6b900";
        uint8_t key24[24], key16[16], ct[24];
        char hex[HEX_ENSIZE(24)];
        size_t i;
        _hex_to_bytes("0123456789ABCDEF23456789ABCDEF01456789ABCDEF0123", key24, 24);
        des_init(&des, (char *)key24, 24, 1, 1);
        for (i = 0; i < 24; i += DES_BLOCK_SIZE) {
            memcpy(ct + i, des_crypt(&des, pt3 + i), DES_BLOCK_SIZE);
        }
        tohex((char *)ct, 24, hex, 1);
        CuAssertStrEquals(tc, want3, hex);
        des_init(&des, (char *)key24, 24, 1, 0);
        for (i = 0; i < 24; i += DES_BLOCK_SIZE) {
            CuAssertTrue(tc, 0 == memcmp(des_crypt(&des, ct + i), pt3 + i, DES_BLOCK_SIZE));
        }
        _hex_to_bytes("0123456789ABCDEF23456789ABCDEF01", key16, 16);
        des_init(&des, (char *)key16, 16, 1, 1);
        tohex(des_crypt(&des, "abcdefgh"), DES_BLOCK_SIZE, hex, 1);
        CuAssertStrEquals(tc, "328b83dfc96362d5", hex);
    }
}

/* =======================================================================
 * padding —— 各种填充模式
 * ======================================================================= */
static void test_padding(CuTest *tc) {
    uint8_t out[16];

    /* ── ZeroPadding：填充字节为 0 ── */
    ZERO(out, sizeof(out));
    CuAssertIntEquals(tc, ERR_OK, _padding_data(ZeroPadding, "abc", 3, out, 8));
    CuAssertTrue(tc, 0 == memcmp(out, "abc", 3));
    CuAssertTrue(tc, 0 == out[3] && 0 == out[4] && 0 == out[5]
                    && 0 == out[6] && 0 == out[7]);

    /* ── PKCS57：填充字节值等于填充长度 ── */
    ZERO(out, sizeof(out));
    CuAssertIntEquals(tc, ERR_OK, _padding_data(PKCS57, "abc", 3, out, 8));
    CuAssertTrue(tc, 0 == memcmp(out, "abc", 3));
    /* 剩余 5 字节均为 5 */
    for (int i = 3; i < 8; i++) {
        CuAssertTrue(tc, 5 == out[i]);
    }

    /* ── ANSIX923：前置零 + 末尾填充长度 ── */
    ZERO(out, sizeof(out));
    CuAssertIntEquals(tc, ERR_OK, _padding_data(ANSIX923, "ab", 2, out, 8));
    CuAssertTrue(tc, 0 == memcmp(out, "ab", 2));
    /* 中间 5 字节零 */
    for (int i = 2; i < 7; i++) {
        CuAssertTrue(tc, 0 == out[i]);
    }
    /* 末尾字节为填充长度 6 */
    CuAssertTrue(tc, 6 == out[7]);

    /* ── ISO10126：随机字节 + 末尾填充长度 ── */
    ZERO(out, sizeof(out));
    // 取随机字节现在走返回值，不再 abort，成功路径必须是 ERR_OK
    CuAssertIntEquals(tc, ERR_OK, _padding_data(ISO10126, "ab", 2, out, 8));
    CuAssertTrue(tc, 0 == memcmp(out, "ab", 2));
    /* 末尾字节为填充长度 */
    CuAssertTrue(tc, 6 == out[7]);

    /* ── 输入长度等于要求长度：原样拷贝，不填充 ── */
    uint8_t out2[8];
    ZERO(out2, sizeof(out2));
    CuAssertIntEquals(tc, ERR_OK, _padding_data(PKCS57, "12345678", 8, out2, 8));
    CuAssertTrue(tc, 0 == memcmp(out2, "12345678", 8));

    /* ── 输入长度 > 要求长度：装不下，返 ERR_FAILED 且一个字节都不写。
     * 改前返回 void，调用方拿到的是未初始化的 output 且无从判别 ── */
    uint8_t out3[4];
    ZERO(out3, sizeof(out3));
    CuAssertIntEquals(tc, ERR_FAILED, _padding_data(PKCS57, "abcdef", 6, out3, 4));
    for (int i = 0; i < 4; i++) {
        CuAssertTrue(tc, 0 == out3[i]);
    }

    /* ── dlens 非 0 却传 NULL data：改前 output 推不动而 remain 照减，
     * 填充落到 out[0..] 而不是 out[dlens..]，且仍返 ERR_OK ── */
    uint8_t out4[8];
    ZERO(out4, sizeof(out4));
    CuAssertIntEquals(tc, ERR_FAILED, _padding_data(PKCS57, NULL, 2, out4, 8));
    for (int i = 0; i < 8; i++) {
        CuAssertTrue(tc, 0 == out4[i]);
    }
    /* 对照：dlens 为 0 时 NULL 合法，整块都是填充 */
    ZERO(out4, sizeof(out4));
    CuAssertIntEquals(tc, ERR_OK, _padding_data(PKCS57, NULL, 0, out4, 8));
    for (int i = 0; i < 8; i++) {
        CuAssertTrue(tc, 8 == out4[i]);
    }

    /* ── _padding_key：klens < reqlens 时填充零并返回 pdkey ── */
    uint8_t pdkey[16];
    ZERO(pdkey, sizeof(pdkey));
    uint8_t *r = _padding_key("key", 3, pdkey, 8);
    CuAssertTrue(tc, r == pdkey);
    CuAssertTrue(tc, 0 == memcmp(r, "key", 3));
    /* 后续填充为零 */
    for (int i = 3; i < 8; i++) {
        CuAssertTrue(tc, 0 == r[i]);
    }

    /* ── _padding_key：klens >= reqlens 时直接返回原 key 指针 ── */
    const char *long_key = "this_is_a_longer_key";
    uint8_t *r2 = _padding_key(long_key, strlen(long_key), pdkey, 8);
    /* 返回值指向原 key，不复制 */
    CuAssertTrue(tc, (uint8_t *)long_key == r2);
}

// padding 各模式补充：16/32 byte block + NoPadding + dlens=0 + ISO10126 末位
static void test_padding_extra(CuTest *tc) {
    // ── PKCS57 在 16-byte block 边界（AES）──
    uint8_t out16[16];
    ZERO(out16, sizeof(out16));
    CuAssertIntEquals(tc, ERR_OK, _padding_data(PKCS57, "hello", 5, out16, 16));
    CuAssertTrue(tc, 0 == memcmp(out16, "hello", 5));
    // 剩 11 字节都应该是 11 (0x0B)
    for (int i = 5; i < 16; i++) {
        CuAssertIntEquals(tc, 11, out16[i]);
    }

    // ── PKCS57 dlens=0：整 block 都填充 reqlens 字节 ──
    uint8_t pblock[16];
    ZERO(pblock, sizeof(pblock));
    CuAssertIntEquals(tc, ERR_OK, _padding_data(PKCS57, NULL, 0, pblock, 16));
    for (int i = 0; i < 16; i++) {
        CuAssertIntEquals(tc, 16, pblock[i]);
    }

    // ── ZeroPadding 在 32-byte block ──
    uint8_t out32[32];
    memset(out32, 0xff, sizeof(out32));
    CuAssertIntEquals(tc, ERR_OK, _padding_data(ZeroPadding, "abcd", 4, out32, 32));
    CuAssertTrue(tc, 0 == memcmp(out32, "abcd", 4));
    for (int i = 4; i < 32; i++) {
        CuAssertIntEquals(tc, 0, out32[i]);
    }

    // ── ANSIX923 在 16-byte block 边界 ──
    uint8_t ansi[16];
    memset(ansi, 0xff, sizeof(ansi));
    CuAssertIntEquals(tc, ERR_OK, _padding_data(ANSIX923, "ABC", 3, ansi, 16));
    CuAssertTrue(tc, 0 == memcmp(ansi, "ABC", 3));
    // 中间 12 字节为 0
    for (int i = 3; i < 15; i++) {
        CuAssertIntEquals(tc, 0, ansi[i]);
    }
    // 最后字节为填充长度 13
    CuAssertIntEquals(tc, 13, ansi[15]);

    // ── ISO10126：末尾字节为 padlen，前面字节随机（不验证值，仅验证末尾）──
    uint8_t iso[16];
    CuAssertIntEquals(tc, ERR_OK, _padding_data(ISO10126, "X", 1, iso, 16));
    CuAssertIntEquals(tc, 'X', iso[0]);
    CuAssertIntEquals(tc, 15, iso[15]);

    // ── NoPadding：默认分支不写 padding 字节（保持原状）──
    uint8_t nopad[16];
    memset(nopad, 0xab, sizeof(nopad));
    CuAssertIntEquals(tc, ERR_OK, _padding_data(NoPadding, "xy", 2, nopad, 16));
    // data 部分已拷贝
    CuAssertTrue(tc, 0 == memcmp(nopad, "xy", 2));
    // padding 区域应保留初始值 0xab（NoPadding 不修改）
    for (int i = 2; i < 16; i++) {
        CuAssertIntEquals(tc, 0xab, nopad[i]);
    }

    // ── _padding_key：reqlens=16/24/32 不同 AES keybits ──
    uint8_t k16[16], k24[24], k32[32];
    const char *raw = "passwd";
    size_t rlen = strlen(raw);
    ZERO(k16, sizeof(k16)); ZERO(k24, sizeof(k24)); ZERO(k32, sizeof(k32));
    CuAssertTrue(tc, k16 == _padding_key(raw, rlen, k16, 16));
    CuAssertTrue(tc, k24 == _padding_key(raw, rlen, k24, 24));
    CuAssertTrue(tc, k32 == _padding_key(raw, rlen, k32, 32));
    CuAssertTrue(tc, 0 == memcmp(k16, raw, rlen));
    CuAssertTrue(tc, 0 == memcmp(k24, raw, rlen));
    CuAssertTrue(tc, 0 == memcmp(k32, raw, rlen));
    // 尾部零填充
    for (int i = (int)rlen; i < 16; i++) {
        CuAssertIntEquals(tc, 0, k16[i]);
    }
    for (int i = (int)rlen; i < 24; i++) {
        CuAssertIntEquals(tc, 0, k24[i]);
    }
    for (int i = (int)rlen; i < 32; i++) {
        CuAssertIntEquals(tc, 0, k32[i]);
    }
}

/* =======================================================================
 * cipher_block / cipher_reset —— 分组级 API 与 IV 重置
 * cipher_block 直接对单个分组加解密；
 * cipher_reset 将 cur_iv 重置为初始 iv，CTR/CFB/OFB 流模式下复用同一 ctx 必须先 reset
 * ======================================================================= */
// key / iv 传 NULL + 长度 0: _padding_key 不能拿 NULL 去 memcpy(UBSan 的 nonnull-attribute
// 会报), 语义上等价于全零 key + 全零 IV, 这里按"与显式全零等价"来断言
static void test_cipher_null_key_iv(CuTest *tc) {
    char zero16[16];
    memset(zero16, 0, sizeof(zero16));
    const char *blk = "0123456789abcdef";
    size_t n1, n2;
    char c1[16];

    cipher_ctx a;
    cipher_init(&a, AES, CBC, NULL, 0, 128, 1);
    cipher_iv(&a, NULL, 0);
    void *p = cipher_block(&a, blk, 16, &n1);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, 16 == n1);
    memcpy(c1, p, 16);
    cipher_free(&a);

    cipher_ctx b;
    cipher_init(&b, AES, CBC, zero16, sizeof(zero16), 128, 1);
    cipher_iv(&b, zero16, sizeof(zero16));
    p = cipher_block(&b, blk, 16, &n2);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, 16 == n2);
    CuAssertTrue(tc, 0 == memcmp(c1, p, 16));
    cipher_free(&b);
}

static void test_cipher_block_reset(CuTest *tc) {
    const char *key16 = "0123456789abcdef";
    const char *iv16 = "abcdef0123456789";
    cipher_ctx enc;

    /* AES-128 CTR：cipher_block 直接处理 16 字节分组 */
    cipher_init(&enc, AES, CTR, key16, 16, 128, 1);
    cipher_iv(&enc, iv16, 16);
    const char *blk_a = "0000000000000000";
    const char *blk_b = "1111111111111111";
    size_t s1, s2, s3;
    /* 拷贝 cipher_block 的输出（指针指向 ctx 内部缓冲，下次调用会覆盖） */
    char c1[16], c2[16], c3[16];
    void *p = cipher_block(&enc, blk_a, 16, &s1);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, 16 == s1);
    memcpy(c1, p, 16);
    p = cipher_block(&enc, blk_b, 16, &s2);
    CuAssertPtrNotNull(tc, p);
    memcpy(c2, p, 16);
    /* CTR 计数器已递增，相同 blk_a 第二次加密结果应不同 */
    p = cipher_block(&enc, blk_a, 16, &s3);
    memcpy(c3, p, 16);
    CuAssertTrue(tc, 0 != memcmp(c1, c3, 16));

    /* cipher_reset 后 cur_iv 复原，再次加密 blk_a 应得到 c1 */
    cipher_reset(&enc);
    p = cipher_block(&enc, blk_a, 16, &s1);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, 0 == memcmp(c1, p, 16));
    cipher_free(&enc);

    /* ECB + NoPadding 下，分组长度不足时 cipher_block 返回 NULL */
    cipher_init(&enc, AES, ECB, key16, 16, 128, 1);
    p = cipher_block(&enc, "short", 5, &s1);
    CuAssertTrue(tc, NULL == p);
    cipher_free(&enc);

    /* CFB 模式：reset 验证 cur_iv 回到初始 iv */
    cipher_ctx cfb;
    cipher_init(&cfb, AES, CFB, key16, 16, 128, 1);
    cipher_iv(&cfb, iv16, 16);
    p = cipher_block(&cfb, blk_a, 16, &s1);
    memcpy(c1, p, 16);
    /* CFB 加密后 cur_iv 应更新为密文 */
    CuAssertTrue(tc, 0 != memcmp(cfb.cur_iv, cfb.iv, 16));
    cipher_reset(&cfb);
    /* reset 后 cur_iv 与 iv 一致 */
    CuAssertTrue(tc, 0 == memcmp(cfb.cur_iv, cfb.iv, 16));
    cipher_free(&cfb);

    /* OFB 模式：reset 后再次加密 blk_a 应得到首次同样密文 */
    cipher_ctx ofb;
    cipher_init(&ofb, AES, OFB, key16, 16, 128, 1);
    cipher_iv(&ofb, iv16, 16);
    p = cipher_block(&ofb, blk_a, 16, &s1);
    memcpy(c1, p, 16);
    p = cipher_block(&ofb, blk_b, 16, &s2);
    CuAssertPtrNotNull(tc, p);
    cipher_reset(&ofb);
    p = cipher_block(&ofb, blk_a, 16, &s1);
    CuAssertTrue(tc, 0 == memcmp(c1, p, 16));
    cipher_free(&ofb);
}

/* =======================================================================
 * cipher 流模式解密方向 —— CFB/OFB/CTR 在 encrypt=0 时同样须建正向密钥表。
 * cipher_init 里 fwdkey 只在"解密 + 流模式"这一格才为真，是该谓词唯一非平凡的输入组合；
 * 若取反，aes_init / des_init 会去建逆向轮密钥，往返得不到原文而全程无任何报错。
 * cipher_block 返回 ctx 内部缓冲指针，故每段都在 cipher_free 之前 memcpy 出来
 * ======================================================================= */
static void test_cipher_stream_decrypt(CuTest *tc) {
    const char *key16 = "0123456789abcdef";
    const char *iv16 = "abcdef0123456789";
    const char *plain = "Hello, Cipher!!!";
    const char *des_key = "8bytekey";
    const char *iv8 = "12345678";
    cipher_model modes[] = { CFB, OFB, CTR };
    char enc_buf[64], dec_buf[64];
    cipher_ctx enc, dec;
    size_t elens, dlens;
    void *p;
    int32_t mi;

    for (mi = 0; mi < (int32_t)ARRAY_SIZE(modes); mi++) {
        cipher_init(&enc, AES, modes[mi], key16, 16, 128, 1);
        cipher_iv(&enc, iv16, 16);
        p = cipher_block(&enc, plain, 16, &elens);
        CuAssertPtrNotNull(tc, p);
        CuAssertTrue(tc, 16 == elens);
        memcpy(enc_buf, p, elens);
        cipher_free(&enc);

        cipher_init(&dec, AES, modes[mi], key16, 16, 128, 0);
        cipher_iv(&dec, iv16, 16);
        p = cipher_block(&dec, enc_buf, elens, &dlens);
        CuAssertPtrNotNull(tc, p);
        CuAssertTrue(tc, 16 == dlens);
        memcpy(dec_buf, p, dlens);
        cipher_free(&dec);

        CuAssertTrue(tc, 0 == memcmp(plain, dec_buf, 16));
        CuAssertTrue(tc, 0 != memcmp(plain, enc_buf, 16));
    }

    cipher_init(&enc, DES, CFB, des_key, 8, 0, 1);
    cipher_iv(&enc, iv8, 8);
    p = cipher_block(&enc, plain, 8, &elens);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, 8 == elens);
    memcpy(enc_buf, p, elens);
    cipher_free(&enc);

    cipher_init(&dec, DES, CFB, des_key, 8, 0, 0);
    cipher_iv(&dec, iv8, 8);
    p = cipher_block(&dec, enc_buf, elens, &dlens);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, 8 == dlens);
    memcpy(dec_buf, p, dlens);
    cipher_free(&dec);

    CuAssertTrue(tc, 0 == memcmp(plain, dec_buf, 8));
}

/* =======================================================================
 * cipher_init IV 初始化 —— 未调 cipher_iv 时 iv/cur_iv 必须为全零
 * cipher_ctx 常落在未清零内存上（lcrypt.c 的 cipher.new 用 lua_newuserdata）；
 * 若 cipher_init 不清零 iv/cur_iv，非 ECB 模式会拿堆残留当 IV：
 * 同一 key+明文在不同残留上得到不同密文，加解密对象也互不匹配。
 * cipher_block 这一路不经 cipher_dofinal 的 cipher_reset，是 cur_iv 读先于写的路径
 * ======================================================================= */
static void test_cipher_init_iv_zeroed(CuTest *tc) {
    const char *key16 = "0123456789abcdef";
    const char *plain = "srey cipher iv!!";
    const cipher_model modes[] = { CBC, CFB, OFB, CTR };
    const uint8_t zeroiv[CIPHER_BLOCK_SIZE] = { 0 };
    cipher_ctx dirty_a, dirty_b, clean;
    char out_a[64], out_b[64], out_c[64];
    size_t mi, la, lb, lc;
    void *p;

    for (mi = 0; mi < sizeof(modes) / sizeof(modes[0]); mi++) {
        memset(&dirty_a, 0xAA, sizeof(dirty_a));
        memset(&dirty_b, 0x5C, sizeof(dirty_b));
        memset(&clean, 0x00, sizeof(clean));
        cipher_init(&dirty_a, AES, modes[mi], key16, 16, 128, 1);
        cipher_init(&dirty_b, AES, modes[mi], key16, 16, 128, 1);
        cipher_init(&clean, AES, modes[mi], key16, 16, 128, 1);

        CuAssertTrue(tc, 0 == memcmp(dirty_a.iv, zeroiv, CIPHER_BLOCK_SIZE));
        CuAssertTrue(tc, 0 == memcmp(dirty_a.cur_iv, zeroiv, CIPHER_BLOCK_SIZE));
        CuAssertTrue(tc, 0 == memcmp(dirty_b.iv, zeroiv, CIPHER_BLOCK_SIZE));
        CuAssertTrue(tc, 0 == memcmp(dirty_b.cur_iv, zeroiv, CIPHER_BLOCK_SIZE));

        CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&dirty_a, plain, 16, out_a, &la));
        CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&dirty_b, plain, 16, out_b, &lb));
        CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&clean, plain, 16, out_c, &lc));
        CuAssertTrue(tc, 16 == la);
        CuAssertTrue(tc, la == lb && la == lc);
        CuAssertTrue(tc, 0 == memcmp(out_a, out_b, la));
        CuAssertTrue(tc, 0 == memcmp(out_a, out_c, la));

        cipher_free(&dirty_a);
        cipher_free(&dirty_b);
        cipher_free(&clean);
    }

    memset(&dirty_a, 0xAA, sizeof(dirty_a));
    memset(&dirty_b, 0x5C, sizeof(dirty_b));
    cipher_init(&dirty_a, AES, CBC, key16, 16, 128, 1);
    cipher_init(&dirty_b, AES, CBC, key16, 16, 128, 1);
    p = cipher_block(&dirty_a, plain, 16, &la);
    CuAssertPtrNotNull(tc, p);
    memcpy(out_a, p, la);
    p = cipher_block(&dirty_b, plain, 16, &lb);
    CuAssertPtrNotNull(tc, p);
    CuAssertTrue(tc, la == lb);
    CuAssertTrue(tc, 0 == memcmp(out_a, p, la));
    cipher_free(&dirty_a);
    cipher_free(&dirty_b);
}

/* =======================================================================
 * cipher 流模式完整 round-trip —— CFB / OFB / CTR
 * 现有 test_cipher 仅覆盖 ECB/CBC + PKCS7 整套 dofinal 流程；
 * test_cipher_block_reset 仅触及流模式单分组 cipher_block + reset。
 * 本测试覆盖 cipher_init → cipher_iv → cipher_dofinal 完整流程，跨多种明文长度
 * （非整 block / 整 2 block / 跨 2+1 block）验证流模式密文长度等于明文长度且可还原。
 * ======================================================================= */
static void test_cipher_stream_modes(CuTest *tc) {
    const char *key16 = "0123456789abcdef";
    const char *iv16 = "abcdef0123456789";
    const cipher_model modes[] = { CFB, OFB, CTR };
    const size_t sizes[] = { 17, 32, 33 };
    char plain[64], enc_buf[96], dec_buf[96];
    cipher_ctx enc, dec;
    size_t mi, si, plen, elen, dlen;
    size_t i;

    for (i = 0; i < sizeof(plain); i++) {
        plain[i] = (char)(i * 31 + 7);
    }

    for (mi = 0; mi < sizeof(modes) / sizeof(modes[0]); mi++) {
        for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
            plen = sizes[si];

            cipher_init(&enc, AES, modes[mi], key16, 16, 128, 1);
            cipher_iv(&enc, iv16, 16);
            cipher_init(&dec, AES, modes[mi], key16, 16, 128, 0);
            cipher_iv(&dec, iv16, 16);

            CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&enc, plain, plen, enc_buf, &elen));
            // 流模式：密文长度严格等于明文长度（无填充）
            CuAssertTrue(tc, plen == elen);
            // 密文与明文不同
            CuAssertTrue(tc, 0 != memcmp(plain, enc_buf, plen));

            CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&dec, enc_buf, elen, dec_buf, &dlen));
            CuAssertTrue(tc, plen == dlen);
            CuAssertTrue(tc, 0 == memcmp(plain, dec_buf, plen));

            cipher_free(&enc);
            cipher_free(&dec);
        }
    }
}

/* =======================================================================
 * hmac_free —— 清零敏感密钥派生状态
 * ======================================================================= */
/* 算法属性只留 digest_init 一张表：block_lens(输出长度) / key_block(压缩分组 B) / eng_lens(引擎 ctx 字节数)。
 * 改前 B 由 hmac.c 里第二张按类型特判的表给出，else 一律返 64 且无 default 兜底——
 * 新增算法漏改那里不报错，HMAC 静默算错。本用例逐算法钉死三个值，漏填即挂 */
static void test_digest_attr_table(CuTest *tc) {
    /* abc 一列是各算法的官方向量（RFC 1319/1320/1321 A.5、FIPS 180-1/180-2）。
       只比三个尺寸常量的话，把某一行的三个回调换成别的算法（尺寸相同的 MD4↔MD5、
       SHA256↔SHA1 都可能）完全看不出来，而 digest.new(DIGEST_TYPE.MD2) 从 Lua 可达 */
    /* st/le 两列对 hmac_pbkdf2 的定长快路径：st 为有无 _state，le 为状态字与长度字段是否小端。
       le 填反只会让 PBKDF2 的结果全错，普通摘要照样对，所以单独钉住 */
    struct {
        digest_type t;
        size_t out;
        size_t b;
        size_t eng;
        int32_t st;
        int32_t le;
        const char *abc;
    } want[] = {
        { DG_MD2,    MD2_BLOCK_SIZE,    16,  sizeof(md2_ctx),    0, 0,
          "da853b0d3f88d99b30283a69e6ded6bb" },
        { DG_MD4,    MD4_BLOCK_SIZE,    64,  sizeof(md4_ctx),    1, 1,
          "a448017aaf21d8525fc10ae87aa6729d" },
        { DG_MD5,    MD5_BLOCK_SIZE,    64,  sizeof(md5_ctx),    1, 1,
          "900150983cd24fb0d6963f7d28e17f72" },
        { DG_SHA1,   SHA1_BLOCK_SIZE,   64,  sizeof(sha1_ctx),   1, 0,
          "a9993e364706816aba3e25717850c26c9cd0d89d" },
        { DG_SHA256, SHA256_BLOCK_SIZE, 64,  sizeof(sha256_ctx), 1, 0,
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { DG_SHA512, SHA512_BLOCK_SIZE, 128, sizeof(sha512_ctx), 1, 0,
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
          "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f" },
        /* xxhash：key_block 为 0（hmac_init 据此拒绝），abc 列是 seed=0 的大端 canonical */
        { DG_XXH32,  XXH32_BLOCK_SIZE,  0,   sizeof(xxh32_ctx),  0, 0,
          "32d153ff" },
        { DG_XXH64,  XXH64_BLOCK_SIZE,  0,   sizeof(xxh64_ctx),  0, 0,
          "44bc2cf5ad770999" },
    };
    digest_ctx d;
    char hash[DG_BLOCK_SIZE];
    char hex[HEX_ENSIZE(DG_BLOCK_SIZE)];
    size_t hlen;
    size_t i;
    for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        digest_init(&d, want[i].t);
        CuAssertTrue(tc, want[i].out == d.attr->block_lens);
        CuAssertTrue(tc, want[i].b == d.attr->key_block);
        CuAssertTrue(tc, want[i].eng == d.attr->eng_lens);
        CuAssertTrue(tc, want[i].st == (NULL != d.attr->_state));
        CuAssertTrue(tc, want[i].le == d.attr->islittle);
        /* eng_lens 必须落在联合体内，否则 hmac_reset 的 memcpy 会读写越界 */
        CuAssertTrue(tc, d.attr->eng_lens <= sizeof(d.eng_ctx));
        /* 回调真挂对了才算这一行是对的 */
        digest_update(&d, "abc", 3);
        hlen = digest_final(&d, hash);
        CuAssertTrue(tc, want[i].out == hlen);
        tohex(hash, hlen, hex, 1);
        CuAssertStrEquals(tc, want[i].abc, hex);
        digest_free(&d);
    }
}

static void test_hmac_free(CuTest *tc) {
    hmac_ctx hm;
    char k[32];
    memset(k, 0x42, sizeof(k));
    hmac_init(&hm, DG_SHA256, k, sizeof(k));

    /* init 后 inside_init / outside_init 缓冲非全零 */
    int has_nonzero = 0;
    const uint8_t *raw = (const uint8_t *)&hm;
    for (size_t i = 0; i < sizeof(hmac_ctx); i++) {
        if (0 != raw[i]) {
            has_nonzero = 1;
            break;
        }
    }
    CuAssertTrue(tc, has_nonzero);

    hmac_free(&hm);
    /* hmac_free 后整段 ctx 全零 */
    for (size_t i = 0; i < sizeof(hmac_ctx); i++) {
        CuAssertTrue(tc, 0 == raw[i]);
    }
    /* free 后允许再次 init 不崩溃，结果合法 */
    hmac_init(&hm, DG_SHA256, "k", 1);
    hmac_update(&hm, "abc", 3);
    char hash[DG_BLOCK_SIZE];
    size_t hlen = hmac_final(&hm, hash);
    CuAssertTrue(tc, 32 == hlen);
    hmac_free(&hm);
}

/* hmac_pbkdf2：SHA-1 取 RFC 6070 的向量；SHA-256 的 passwd/salt/1 取 RFC 7914 §11（输出只有第一块，对应 T1 即前 32 字节），
 * 其余（含 SHA-256 的 4096 轮）由 Python hashlib.pbkdf2_hmac 生成。口令长于分组的几条（SHA-512 的 200 字节、
 * MD4/MD5 的 100 字节）要先摘要，短于分组的补零，两条路径各算法都要钉。MD2 那条由按 RFC 1319 独立实现的 MD2 算出。
 * 每条之后在同一 ctx 上再做一次普通 HMAC，须与新建 ctx 的结果一致：快路径改过 inside/outside，返回前必须复位 */
static void test_hmac_pbkdf2(CuTest *tc) {
    static const struct {
        digest_type t;
        size_t plen;
        const char *pwd;
        const char *salt;
        int32_t iter;
        const char *hex;
    } vec[] = {
        { DG_SHA1,   8,   "password", "salt",   1,    "0c60c80f961f0e71f3a9b524af6012062fe037a6" },
        { DG_SHA1,   8,   "password", "salt",   2,    "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957" },
        { DG_SHA1,   8,   "password", "salt",   4096, "4b007901b765489abead49d926f721d065a429c1" },
        { DG_SHA1,   8,   "password", "salt",   0,    "0c60c80f961f0e71f3a9b524af6012062fe037a6" },/* iter<1 按 1 算 */
        { DG_SHA256, 6,   "passwd",   "salt",   1,    "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc" },
        { DG_SHA256, 8,   "password", "salt",   4096, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a" },
        { DG_MD5,    6,   "pencil",   "pepper", 5,    "f8df2855e9684579265df39b551e76e5" },
        { DG_MD5,    100, NULL,       "NaCl",   3,    "e1e51eb5b9d280ff32a0e0b05773c7b3" },
        { DG_MD4,    6,   "pencil",   "pepper", 7,    "b89f63f23543ec563bf821390cb92821" },
        { DG_MD4,    100, NULL,       "NaCl",   3,    "7e489ef8e446ed980086a13c90b3eb91" },
        { DG_SHA512, 200, NULL,       "NaCl",   3,    "459d67075b280a0e3ea741869225c39f8c4ddab81df42747c773d450329d54b2"
                                                      "59e1d6cc5eb86a3f343597f414a30856ec0df0205093d25249acb78cce096f15" },
        { DG_SHA512, 8,   "password", "salt",   2,    "e1d9c16aa681708a45f5c7c4e215ceb66e011a2e9f0040713f18aefdb866d53c"
                                                      "f76cab2868a39b9f7840edce4fef5a82be67335c77a6068e04112754f27ccf4e" },
    };
    char longkey[200];
    char out[DG_BLOCK_SIZE], a[DG_BLOCK_SIZE], b[DG_BLOCK_SIZE], u[DG_BLOCK_SIZE];
    char hex[HEX_ENSIZE(DG_BLOCK_SIZE)];
    const char *pwd;
    static const char one[4] = { 0, 0, 0, 1 };
    hmac_ctx h, fresh;
    size_t i, j, len;
    int32_t k;
    memset(longkey, 'k', sizeof(longkey));
    for (i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
        pwd = (NULL == vec[i].pwd) ? longkey : vec[i].pwd;
        hmac_init(&h, vec[i].t, pwd, vec[i].plen);
        len = hmac_pbkdf2(&h, vec[i].salt, strlen(vec[i].salt), vec[i].iter, out);
        CuAssertTrue(tc, hmac_size(&h) == len);
        tohex(out, len, hex, 1);
        CuAssertStrEquals(tc, vec[i].hex, hex);
        hmac_update(&h, "abc", 3);
        hmac_final(&h, a);
        hmac_init(&fresh, vec[i].t, pwd, vec[i].plen);
        hmac_update(&fresh, "abc", 3);
        hmac_final(&fresh, b);
        CuAssertTrue(tc, 0 == memcmp(a, b, len));
        hmac_free(&fresh);
        hmac_free(&h);
    }
    /* MD2 没有 _state，走逐轮 hmac_update/hmac_final 的回退路径；对照手写的 PBKDF2 */
    hmac_init(&h, DG_MD2, "pencil", 6);
    hmac_update(&h, "pepper", 6);
    hmac_update(&h, one, sizeof(one));
    hmac_final(&h, u);
    memcpy(a, u, MD2_BLOCK_SIZE);
    for (k = 1; k < 3; k++) {
        hmac_update(&h, u, MD2_BLOCK_SIZE);
        hmac_final(&h, u);
        for (j = 0; j < MD2_BLOCK_SIZE; j++) {
            a[j] ^= u[j];
        }
    }
    hmac_free(&h);
    hmac_init(&h, DG_MD2, "pencil", 6);
    CuAssertTrue(tc, MD2_BLOCK_SIZE == hmac_pbkdf2(&h, "pepper", 6, 3, out));
    CuAssertTrue(tc, 0 == memcmp(a, out, MD2_BLOCK_SIZE));
    /* 上面的手写对照也建在库自己的 HMAC-MD2 上，再钉一个外部算出的值 */
    tohex(out, MD2_BLOCK_SIZE, hex, 1);
    CuAssertStrEquals(tc, "c8e79dbe8c1f8f7338dd96d32d7d85cd", hex);
    hmac_free(&h);
}

/* digest_free / cipher_free 的整段清零，写法同 test_hmac_free（ctx 在栈上所以读得到）。
 * 注意这只能覆盖"调用方自己的 ctx"这一层；CLAUDE.md 要求清零的那一串**函数局部栈缓冲**
 * （hmac 的 key_temp/block_ipad/block_opad、scram 的 proof/clientkey/storedkey 等）
 * 返回后栈帧已失效，读它就是 UB，没有可靠的单元测试手段 */
static void test_digest_cipher_free_zeroed(CuTest *tc) {
    digest_ctx d;
    cipher_ctx c;
    const uint8_t *raw;
    int32_t nonzero;
    size_t i;

    digest_init(&d, DG_SHA256);
    digest_update(&d, "abc", 3);
    raw = (const uint8_t *)&d;
    nonzero = 0;
    for (i = 0; i < sizeof(digest_ctx); i++) {
        if (0 != raw[i]) {
            nonzero = 1;
            break;
        }
    }
    CuAssertTrue(tc, 0 != nonzero);
    digest_free(&d);
    for (i = 0; i < sizeof(digest_ctx); i++) {
        CuAssertTrue(tc, 0 == raw[i]);
    }

    cipher_init(&c, AES, CBC, "0123456789abcdef", 16, 128, 1);
    cipher_iv(&c, "abcdef0123456789", 16);
    raw = (const uint8_t *)&c;
    nonzero = 0;
    for (i = 0; i < sizeof(cipher_ctx); i++) {
        if (0 != raw[i]) {
            nonzero = 1;
            break;
        }
    }
    CuAssertTrue(tc, 0 != nonzero);
    cipher_free(&c);
    for (i = 0; i < sizeof(cipher_ctx); i++) {
        CuAssertTrue(tc, 0 == raw[i]);
    }
}

/* =======================================================================
 * base64 非法输入：单字符残组(有效字符 %4==1) / 内嵌非法字符 / '=' 后非 = 字符 返回 0
 * ======================================================================= */
static void test_base64_invalid(CuTest *tc) {
    char out[64];

    /* 空输入、有效字符数 %4==1（单字符残组无法构成字节）返回 0 */
    CuAssertTrue(tc, 0 == bs64_decode("",  0, out));
    CuAssertTrue(tc, 0 == bs64_decode("A", 1, out));

    /* 含非法字符（< '+' 或 > 'z'） */
    CuAssertTrue(tc, 0 == bs64_decode("A!BC", 4, out));/* '!' < '+' */
    CuAssertTrue(tc, 0 == bs64_decode("AB{C", 4, out));/* '{' > 'z' */
    CuAssertTrue(tc, 0 == bs64_decode("AB C", 4, out));/* ' ' < '+' */

    /* '+' 至 'z' 区间内的非 base64 字符（查表 -1） */
    CuAssertTrue(tc, 0 == bs64_decode("A,BC", 4, out));/* ',' 在表中为 -1 */
    CuAssertTrue(tc, 0 == bs64_decode("AB.C", 4, out));/* '.' 在表中为 -1 */

    /* '=' 后接非 '='/CR/LF 字符：伪造截断防御 */
    CuAssertTrue(tc, 0 == bs64_decode("AB=A", 4, out));
    CuAssertTrue(tc, 0 == bs64_decode("A=BC", 4, out));

    /* 无填充 base64（RFC 4648 §3.2）：单字符残组（%4==1）非法 → 0，其余正常解码 */
    CuAssertTrue(tc, 0 == bs64_decode("TWFuT", 5, out));/* 有效字符 % 4 == 1，非法 */

    size_t nlen = bs64_decode("TWFuTW", 6, out);/* % 4 == 2，原 ASan 越界点 */
    CuAssertTrue(tc, 4 == nlen && 0 == memcmp(out, "ManM", 4));
    nlen = bs64_decode("TWFuTWF", 7, out);/* % 4 == 3，原 ASan 越界点 */
    CuAssertTrue(tc, 5 == nlen && 0 == memcmp(out, "ManMa", 5));

    /* 无填充短输入：2 字符→1 字节，3 字符→2 字节 */
    nlen = bs64_decode("TW", 2, out);
    CuAssertTrue(tc, 1 == nlen && 'M' == out[0]);
    nlen = bs64_decode("TWE", 3, out);
    CuAssertTrue(tc, 2 == nlen && 0 == memcmp(out, "Ma", 2));

    /* CR/LF 夹在无填充数据中间：跳过后有效字符为 "TWFu" → "Man" */
    nlen = bs64_decode("TW\r\nFu", 6, out);
    CuAssertTrue(tc, 3 == nlen && 0 == memcmp(out, "Man", 3));

    /* 嵌入 CR/LF 仍允许（多行 base64） */
    const char *with_lf = "Zg==\n";
    size_t dlen = bs64_decode(with_lf, strlen(with_lf), out);
    CuAssertTrue(tc, 1 == dlen);
    CuAssertTrue(tc, 'f' == out[0]);

    /* 拒收时不得留下"合法前缀已解出的那段"：三条拒收路径都要把 out 置空。
       调用方常拿 CALLOC 的缓冲直接 strcmp（smtp 的 AUTH LOGIN 挑战就是），
       留着前缀就等于让 "dXNlcm5hbWU6!!!" 冒充完整的 "username:" */
    memset(out, 0x5a, sizeof(out));
    CuAssertTrue(tc, 0 == bs64_decode("dXNlcm5hbWU6!!!", 15, out));/* 非法字符 */
    CuAssertTrue(tc, '\0' == out[0]);
    memset(out, 0x5a, sizeof(out));
    CuAssertTrue(tc, 0 == bs64_decode("cGFzc3dvcmQ6=X==", 16, out)); /* '=' 后有正文 */
    CuAssertTrue(tc, '\0' == out[0]);
    memset(out, 0x5a, sizeof(out));
    CuAssertTrue(tc, 0 == bs64_decode("TWFuTWFuT", 9, out));/* 尾组只剩 1 个字符 */
    CuAssertTrue(tc, '\0' == out[0]);
}

/* 长输入（期望值由 Python base64 算出）：编码 >= 8 字节才走成组快路径，解码 8 字符成组，
 * 上面 RFC 4648 的向量都短于这个门槛。解码写进恰好 B64DE_SIZE 大的区域，快路径每组多写的那个字节
 * 必须落在界内，其后的哨兵一个都不许被碰 */
static void test_base64_long(CuTest *tc) {
    static const struct {
        size_t n;
        const char *hex;
        const char *b64;
    } vec[] = {
        { 14, "48656c6c6f2c2042617365363421", "SGVsbG8sIEJhc2U2NCE=" },
        { 43, "54686520717569636b2062726f776e20666f78206a756d7073206f76657220746865206c617a7920646f67",
          "VGhlIHF1aWNrIGJyb3duIGZveCBqdW1wcyBvdmVyIHRoZSBsYXp5IGRvZw==" },
        { 50, "078a0d901396199c1fa225a82bae31b437ba3dc043c649cc4fd255d85bde61e467ea6df073f679fc7f0285088b0e9114971a",
          "B4oNkBOWGZwfoiWoK64xtDe6PcBDxknMT9JV2FveYeRn6m3wc/Z5/H8ChQiLDpEUlxo=" },
    };
    uint8_t in[64];
    char enc[B64EN_SIZE(64)];
    char dec[B64DE_SIZE(B64EN_SIZE(64)) + 8];
    size_t i, k, elen, dlen, cap;
    for (i = 0; i < ARRAY_SIZE(vec); i++) {
        _hex_to_bytes(vec[i].hex, in, vec[i].n);
        elen = bs64_encode(in, vec[i].n, enc);
        enc[elen] = '\0';
        CuAssertStrEquals(tc, vec[i].b64, enc);
        cap = B64DE_SIZE(elen);
        memset(dec, 0xA5, sizeof(dec));
        dlen = bs64_decode(enc, elen, dec);
        CuAssertTrue(tc, vec[i].n == dlen);
        CuAssertTrue(tc, 0 == memcmp(dec, in, dlen));
        for (k = cap; k < sizeof(dec); k++) {
            CuAssertTrue(tc, (char)0xA5 == dec[k]);
        }
    }
}

/* =======================================================================
 * digest / hmac 的 final 之后可直接开始下一条消息
 * 各引擎 *_final 末尾都 secure_zero 掉自己的 ctx，不重建 IV 的话第二次 final
 * 是"全零 IV 算空消息"，得到一个与输入、与密钥都无关的常量
 * ======================================================================= */
static void test_digest_hmac_final_resets(CuTest *tc) {
    char h1[DG_BLOCK_SIZE], h2[DG_BLOCK_SIZE];
    size_t l1, l2;

    /* digest：final 后直接跑第二条消息，须与全新上下文一致 */
    digest_ctx d, fresh;
    digest_init(&d, DG_SHA256);
    digest_update(&d, "abc", 3);
    l1 = digest_final(&d, h1);
    digest_update(&d, "hello world", 11);
    l1 = digest_final(&d, h1);
    digest_init(&fresh, DG_SHA256);
    digest_update(&fresh, "hello world", 11);
    l2 = digest_final(&fresh, h2);
    CuAssertTrue(tc, l1 == l2 && 0 == memcmp(h1, h2, l1));

    /* 连着两次 final：第二次是空消息的摘要，不是与输入无关的常量 */
    digest_init(&d, DG_SHA256);
    digest_update(&d, "abc", 3);
    digest_final(&d, h1);
    l1 = digest_final(&d, h1);
    digest_init(&fresh, DG_SHA256);
    l2 = digest_final(&fresh, h2);
    CuAssertTrue(tc, l1 == l2 && 0 == memcmp(h1, h2, l1));

    /* hmac：final 后密钥仍在。不 reset 直接算同一条消息，须得到同一个 tag
       （修复前这里对任何密钥都返回同一个常量） */
    hmac_ctx m;
    hmac_init(&m, DG_SHA256, "key", 3);
    hmac_update(&m, "Hi There", 8);
    l1 = hmac_final(&m, h1);
    hmac_update(&m, "Hi There", 8);
    l2 = hmac_final(&m, h2);
    CuAssertTrue(tc, l1 == l2 && 0 == memcmp(h1, h2, l1));

    /* 不同密钥的第二次 final 必须不同 */
    hmac_ctx m2;
    hmac_init(&m2, DG_SHA256, "COMPLETELY-OTHER-KEY", 20);
    hmac_update(&m2, "Hi There", 8);
    hmac_final(&m2, h2);
    hmac_update(&m2, "Hi There", 8);
    hmac_final(&m2, h2);
    CuAssertTrue(tc, 0 != memcmp(h1, h2, l1));

    hmac_free(&m);
    hmac_free(&m2);
    digest_free(&d);
    digest_free(&fresh);
}

/* =======================================================================
 * cipher_dofinal 的"失败"与"结果长度 0"必须分得开：
 * 加密空明文再解回来本就是 0 字节，与填充校验失败同为 0，靠返回值区分
 * ======================================================================= */
static void test_cipher_dofinal_empty_vs_fail(CuTest *tc) {
    const char key[16] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                           0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10 };
    char ct[64], pt[64];
    size_t ctlen, ptlen;

    cipher_ctx enc, dec;
    cipher_init(&enc, AES, ECB, key, sizeof(key), 128, 1);
    cipher_padding(&enc, PKCS57);
    CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&enc, "", 0, ct, &ctlen));
    CuAssertTrue(tc, 16 == ctlen);// 空明文也产出一个整填充块

    /* 合法密文 → ERR_OK 且长度 0 */
    cipher_init(&dec, AES, ECB, key, sizeof(key), 128, 0);
    cipher_padding(&dec, PKCS57);
    CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&dec, ct, ctlen, pt, &ptlen));
    CuAssertTrue(tc, 0 == ptlen);

    /* 篡改一个字节 → ERR_FAILED，长度同样是 0 */
    ct[0] = (char)(ct[0] ^ 0xFF);
    CuAssertIntEquals(tc, ERR_FAILED, cipher_dofinal(&dec, ct, ctlen, pt, &ptlen));
    CuAssertTrue(tc, 0 == ptlen);

    cipher_free(&enc);
    cipher_free(&dec);
}

/* =======================================================================
 * SCRAM：服务端 r= 恰好等于客户端 nonce（一个随机字节都没贡献）须被拒
 * ======================================================================= */
static void test_scram_server_nonce_required(CuTest *tc) {
    const char *cli_nonce = "fyko+d2lbbFgONRv9qkxdawL";
    char srv_first[256];

    scram_ctx *cli = scram_init("SCRAM-SHA-1", 1);
    CuAssertPtrNotNull(tc, cli);
    scram_set_user(cli, "user", 4);
    scram_set_pwd(cli, "pencil", 6);
    char *first = scram_first_message(cli);
    FREE(first);
    _scram_inject_nonce(cli, cli_nonce, "user");

    /* r= 原样回灌：前缀校验能过，但服务端 nonce 为空 */
    SNPRINTF(srv_first, sizeof(srv_first), "r=%s,s=QSXCR+Q6sek8bf92,i=4096", cli_nonce);
    CuAssertIntEquals(tc, ERR_FAILED,
        scram_parse_first_message(cli, srv_first, strlen(srv_first)));
    scram_free(cli);

    /* 只要多一个字符就合法 */
    cli = scram_init("SCRAM-SHA-1", 1);
    scram_set_user(cli, "user", 4);
    scram_set_pwd(cli, "pencil", 6);
    first = scram_first_message(cli);
    FREE(first);
    _scram_inject_nonce(cli, cli_nonce, "user");
    SNPRINTF(srv_first, sizeof(srv_first), "r=%sX,s=QSXCR+Q6sek8bf92,i=4096", cli_nonce);
    CuAssertIntEquals(tc, ERR_OK,
        scram_parse_first_message(cli, srv_first, strlen(srv_first)));
    scram_free(cli);
}

/* =======================================================================
 * urlraw 非法输入：%GG / %1 / 末尾孤悬 %  解码器宽容处理（不转换 % 原样保留）
 * ======================================================================= */
static void test_urlraw_invalid(CuTest *tc) {
    char buf[64];
    size_t dlen;

    /* %GG：G 非十六进制，url_decode 不做转换，% G G 原样保留 */
    safe_fill_str(buf, sizeof(buf), "a%GGb");
    dlen = url_decode(buf, strlen("a%GGb"), 0);
    CuAssertTrue(tc, 5 == dlen);
    CuAssertTrue(tc, 0 == memcmp(buf, "a%GGb", 5));

    /* 只有 %1：剩余字节不够 2 位，原样保留 */
    safe_fill_str(buf, sizeof(buf), "x%1");
    dlen = url_decode(buf, strlen("x%1"), 0);
    CuAssertTrue(tc, 3 == dlen);
    CuAssertTrue(tc, 0 == memcmp(buf, "x%1", 3));

    /* 末尾孤悬 % */
    safe_fill_str(buf, sizeof(buf), "abc%");
    dlen = url_decode(buf, strlen("abc%"), 0);
    CuAssertTrue(tc, 4 == dlen);
    CuAssertTrue(tc, 0 == memcmp(buf, "abc%", 4));

    /* 半合法：%1G —— 'G' 非 hex，整段保留 */
    safe_fill_str(buf, sizeof(buf), "%1G");
    dlen = url_decode(buf, strlen("%1G"), 0);
    CuAssertTrue(tc, 3 == dlen);
    CuAssertTrue(tc, 0 == memcmp(buf, "%1G", 3));

    /* 大小写混合 hex 正常解码 */
    safe_fill_str(buf, sizeof(buf), "%aA");
    dlen = url_decode(buf, strlen("%aA"), 0);
    CuAssertTrue(tc, 1 == dlen);
    CuAssertTrue(tc, (char)0xaa == buf[0]);

    /* + 转空格（编码反向语义） */
    safe_fill_str(buf, sizeof(buf), "a+b+c");
    dlen = url_decode(buf, strlen("a+b+c"), 1);
    CuAssertTrue(tc, 5 == dlen);
    CuAssertTrue(tc, 0 == memcmp(buf, "a b c", dlen));

    /* % 后跟高位字节（≥0x80）：非 hex，整段原样保留（isxdigit 入参须按 unsigned char 处理）*/
    buf[0] = '%'; buf[1] = (char)0x80; buf[2] = (char)0x81; buf[3] = 'z';
    dlen = url_decode(buf, 4, 0);
    CuAssertTrue(tc, 4 == dlen);
    CuAssertTrue(tc, '%' == buf[0] && (char)0x80 == buf[1] && (char)0x81 == buf[2] && 'z' == buf[3]);
}

/* ======================================================================= */

/* 空用户名：RFC 5802 允许 n= 为空（libpq 恒发这一形态，用户名走启动包）。
 * 客户端不设 user 时发的就是它，服务端必须能解析并走完整套握手 */
static void test_scram_empty_user(CuTest *tc) {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
    scram_ctx *srv = scram_init("SCRAM-SHA-256", 0);
    CuAssertPtrNotNull(tc, cli);
    CuAssertPtrNotNull(tc, srv);
    scram_set_pwd(cli, "pass", 4);
    scram_set_pwd(srv, "pass", 4);
    scram_set_salt(srv, (char *)_SALT16, sizeof(_SALT16));
    scram_set_iter(srv, 4096);

    char *cf = scram_first_message(cli);
    CuAssertPtrNotNull(tc, cf);
    CuAssertPtrNotNull(tc, strstr(cf, "n,,n=,r="));/* 空用户名的线上形态 */
    CuAssertIntEquals(tc, ERR_OK, scram_parse_first_message(srv, cf, strlen(cf)));
    CuAssertStrEquals(tc, "", scram_get_user(srv));
    FREE(cf);

    char *sf = scram_first_message(srv);
    CuAssertPtrNotNull(tc, sf);
    CuAssertIntEquals(tc, ERR_OK, scram_parse_first_message(cli, sf, strlen(sf)));
    FREE(sf);

    char *clf = scram_final_message(cli);
    CuAssertPtrNotNull(tc, clf);
    CuAssertIntEquals(tc, ERR_OK, scram_check_final_message(srv, clf, strlen(clf)));
    _scram_free_msg(&clf);

    char *svf = scram_final_message(srv);
    CuAssertPtrNotNull(tc, svf);
    CuAssertIntEquals(tc, ERR_OK, scram_check_final_message(cli, svf, strlen(svf)));
    _scram_free_msg(&svf);

    /* 对外 setter 仍拦空:那是误用守卫,与协议侧的合法空值是两回事 */
    CuAssertIntEquals(tc, ERR_FAILED, scram_set_user(cli, "", 0));
    CuAssertIntEquals(tc, ERR_FAILED, scram_set_user(cli, NULL, 0));
    scram_free(cli);
    scram_free(srv);
}

/* 3DES 短密钥补齐：klens 为 0 时 key 允许是 NULL（单 DES 分支一直如此）。
 * ASan/UBSan 下才看得出来——memcpy 不收 NULL，哪怕长度是 0 */
static void test_des3_null_key(CuTest *tc) {
    cipher_ctx c;
    char in[8] = { 0 };
    char out[16];
    size_t olens = 0;
    cipher_init(&c, DES3, ECB, NULL, 0, 0, 1);
    cipher_padding(&c, NoPadding);
    CuAssertIntEquals(tc, ERR_OK, cipher_dofinal(&c, in, sizeof(in), out, &olens));
    CuAssertTrue(tc, sizeof(in) == olens);
    cipher_free(&c);
}

/* =======================================================================
 * xxhash —— 官方向量、流式与一次性一致、非对齐、(NULL,0)、digest 路径
 * ======================================================================= */
// 测试数据生成器（同 xxHash cli/xsum_sanity_check.c）：gen 从 PRIME32 起，每字节取最高 8 位后乘 PRIME64
static const uint8_t *_xxh_testbuf(void) {
    uint64_t gen = 0x9E3779B1ULL;
    size_t i;
    for (i = 0; i < XXH_TESTBUF_LENS; i++) {
        _xxh_buf[i] = (uint8_t)(gen >> 56);
        gen *= 0x9E3779B185EBCA8DULL;
    }
    return _xxh_buf;
}
// 按 chunk 分块流式计算；每次 update 后都 digest 一次并与前缀的一次性结果比对，
// 既验证中途 digest 不改 ctx（之后还能继续 update），也验证各缓冲边界
static uint32_t _xxh32_stream(CuTest *tc, const uint8_t *buf, size_t len, uint32_t seed, size_t chunk) {
    xxh32_ctx ctx;
    size_t off = 0;
    size_t n;
    xxh32_init(&ctx, seed);
    while (off < len) {
        n = len - off < chunk ? len - off : chunk;
        xxh32_update(&ctx, buf + off, n);
        off += n;
        CuAssertTrue(tc, xxh32(buf, off, seed) == xxh32_digest(&ctx));
    }
    return xxh32_digest(&ctx);
}
static uint64_t _xxh64_stream(CuTest *tc, const uint8_t *buf, size_t len, uint64_t seed, size_t chunk) {
    xxh64_ctx ctx;
    size_t off = 0;
    size_t n;
    xxh64_init(&ctx, seed);
    while (off < len) {
        n = len - off < chunk ? len - off : chunk;
        xxh64_update(&ctx, buf + off, n);
        off += n;
        CuAssertTrue(tc, xxh64(buf, off, seed) == xxh64_digest(&ctx));
    }
    return xxh64_digest(&ctx);
}
// 同一个 xxh3_ctx 同时出 64 位与 128 位结果
static void _xxh3_stream(CuTest *tc, const uint8_t *buf, size_t len, uint64_t seed, size_t chunk,
    uint64_t *h64, xxh128_t *h128) {
    xxh3_ctx ctx;
    xxh128_t a, b;
    size_t off = 0;
    size_t n;
    xxh3_init(&ctx, seed);
    while (off < len) {
        n = len - off < chunk ? len - off : chunk;
        xxh3_update(&ctx, buf + off, n);
        off += n;
        CuAssertTrue(tc, xxh3_64(buf, off, seed) == xxh3_digest64(&ctx));
        a = xxh3_128(buf, off, seed);
        b = xxh3_digest128(&ctx);
        CuAssertTrue(tc, a.low == b.low && a.high == b.high);
    }
    *h64 = xxh3_digest64(&ctx);
    *h128 = xxh3_digest128(&ctx);
}
static void test_xxh32_vectors(CuTest *tc) {
    const uint8_t *buf = _xxh_testbuf();
    char msg[64];
    size_t i, c;
    for (i = 0; i < ARRAY_SIZE(_XXH32_VEC); i++) {
        SNPRINTF(msg, sizeof(msg), "xxh32 len=%u seed=%08x", _XXH32_VEC[i].len, _XXH32_VEC[i].seed);
        CuAssert(tc, msg, _XXH32_VEC[i].h == xxh32(buf, _XXH32_VEC[i].len, _XXH32_VEC[i].seed));
        for (c = 0; c < ARRAY_SIZE(_XXH_CHUNKS); c++) {
            CuAssert(tc, msg, _XXH32_VEC[i].h == _xxh32_stream(tc, buf, _XXH32_VEC[i].len, _XXH32_VEC[i].seed, _XXH_CHUNKS[c]));
        }
    }
}
static void test_xxh64_vectors(CuTest *tc) {
    const uint8_t *buf = _xxh_testbuf();
    char msg[64];
    size_t i, c;
    for (i = 0; i < ARRAY_SIZE(_XXH64_VEC); i++) {
        SNPRINTF(msg, sizeof(msg), "xxh64 len=%u seed=%016llx", _XXH64_VEC[i].len, (unsigned long long)_XXH64_VEC[i].seed);
        CuAssert(tc, msg, _XXH64_VEC[i].h == xxh64(buf, _XXH64_VEC[i].len, _XXH64_VEC[i].seed));
        for (c = 0; c < ARRAY_SIZE(_XXH_CHUNKS); c++) {
            CuAssert(tc, msg, _XXH64_VEC[i].h == _xxh64_stream(tc, buf, _XXH64_VEC[i].len, _XXH64_VEC[i].seed, _XXH_CHUNKS[c]));
        }
    }
}
static void test_xxh3_64_vectors(CuTest *tc) {
    const uint8_t *buf = _xxh_testbuf();
    char msg[64];
    uint64_t h64;
    xxh128_t h128;
    size_t i, c;
    for (i = 0; i < ARRAY_SIZE(_XXH3_64_VEC); i++) {
        SNPRINTF(msg, sizeof(msg), "xxh3_64 len=%u seed=%016llx", _XXH3_64_VEC[i].len, (unsigned long long)_XXH3_64_VEC[i].seed);
        CuAssert(tc, msg, _XXH3_64_VEC[i].h == xxh3_64(buf, _XXH3_64_VEC[i].len, _XXH3_64_VEC[i].seed));
        for (c = 0; c < ARRAY_SIZE(_XXH_CHUNKS); c++) {
            _xxh3_stream(tc, buf, _XXH3_64_VEC[i].len, _XXH3_64_VEC[i].seed, _XXH_CHUNKS[c], &h64, &h128);
            CuAssert(tc, msg, _XXH3_64_VEC[i].h == h64);
        }
    }
}
static void test_xxh3_128_vectors(CuTest *tc) {
    const uint8_t *buf = _xxh_testbuf();
    char msg[64];
    uint64_t h64;
    xxh128_t h128;
    size_t i, c;
    for (i = 0; i < ARRAY_SIZE(_XXH3_128_VEC); i++) {
        SNPRINTF(msg, sizeof(msg), "xxh3_128 len=%u seed=%016llx", _XXH3_128_VEC[i].len, (unsigned long long)_XXH3_128_VEC[i].seed);
        h128 = xxh3_128(buf, _XXH3_128_VEC[i].len, _XXH3_128_VEC[i].seed);
        CuAssert(tc, msg, _XXH3_128_VEC[i].low == h128.low && _XXH3_128_VEC[i].high == h128.high);
        for (c = 0; c < ARRAY_SIZE(_XXH_CHUNKS); c++) {
            _xxh3_stream(tc, buf, _XXH3_128_VEC[i].len, _XXH3_128_VEC[i].seed, _XXH_CHUNKS[c], &h64, &h128);
            CuAssert(tc, msg, _XXH3_128_VEC[i].low == h128.low && _XXH3_128_VEC[i].high == h128.high);
        }
    }
}
/* init 只复位会被读到的字段（xxh3 的 buf 不清零，xxh32/64 只清 acc 之前的字段），
 * 所以每个 seed 先把 ctx 整个填成垃圾，再在同一 ctx 上依次跑 11 条不同长度的流（每条前 1/3 与余下分两次喂），
 * 前一条流的残留与最初的垃圾都不许影响结果，每条都得等于一次性计算 */
static void test_xxh_init_garbage(CuTest *tc) {
    const size_t lens[] = { 0, 1, 15, 16, 31, 32, 33, 100, 240, 241, 1025 };
    const uint64_t seeds[] = { 0, 0x1234567890ABCDEFULL };
    const uint8_t *buf = _xxh_testbuf();
    xxh32_ctx c32;
    xxh64_ctx c64;
    xxh3_ctx c3;
    xxh128_t a, b;
    size_t i, k, cut;
    for (k = 0; k < ARRAY_SIZE(seeds); k++) {
        memset(&c32, 0xA5, sizeof(c32));
        memset(&c64, 0xA5, sizeof(c64));
        memset(&c3, 0xA5, sizeof(c3));
        for (i = 0; i < ARRAY_SIZE(lens); i++) {
            cut = lens[i] / 3;
            xxh32_init(&c32, (uint32_t)seeds[k]);
            xxh32_update(&c32, buf, cut);
            xxh32_update(&c32, buf + cut, lens[i] - cut);
            CuAssertTrue(tc, xxh32(buf, lens[i], (uint32_t)seeds[k]) == xxh32_digest(&c32));
            xxh64_init(&c64, seeds[k]);
            xxh64_update(&c64, buf, cut);
            xxh64_update(&c64, buf + cut, lens[i] - cut);
            CuAssertTrue(tc, xxh64(buf, lens[i], seeds[k]) == xxh64_digest(&c64));
            xxh3_init(&c3, seeds[k]);
            xxh3_update(&c3, buf, cut);
            xxh3_update(&c3, buf + cut, lens[i] - cut);
            CuAssertTrue(tc, xxh3_64(buf, lens[i], seeds[k]) == xxh3_digest64(&c3));
            a = xxh3_128(buf, lens[i], seeds[k]);
            b = xxh3_digest128(&c3);
            CuAssertTrue(tc, a.low == b.low && a.high == b.high);
        }
    }
}
static void test_xxh_misc(CuTest *tc) {
    // 覆盖各算法的短路径、中键路径和 XXH3 长路径（>240）
    const size_t lens[] = { 0, 3, 15, 17, 33, 129, 241, 1025, XXH_TESTBUF_LENS - 1 };
    const uint8_t *buf = _xxh_testbuf();
    uint64_t al[XXH_TESTBUF_LENS / 8 + 1];
    xxh32_ctx c32;
    xxh64_ctx c64;
    xxh3_ctx c3;
    digest_ctx d;
    char out[DG_BLOCK_SIZE];
    char want[XXH64_BLOCK_SIZE];
    xxh128_t a, b;
    size_t i;
    /* 非对齐：从 buf+1 算，要等于先 memmove 到对齐地址再算 */
    for (i = 0; i < ARRAY_SIZE(lens); i++) {
        memmove(al, buf + 1, lens[i]);
        CuAssertTrue(tc, xxh32(buf + 1, lens[i], 7) == xxh32(al, lens[i], 7));
        CuAssertTrue(tc, xxh64(buf + 1, lens[i], 7) == xxh64(al, lens[i], 7));
        CuAssertTrue(tc, xxh3_64(buf + 1, lens[i], 7) == xxh3_64(al, lens[i], 7));
        a = xxh3_128(buf + 1, lens[i], 7);
        b = xxh3_128(al, lens[i], 7);
        CuAssertTrue(tc, a.low == b.low && a.high == b.high);
    }
    /* (NULL, 0) 等于空串，一次性与流式都不对 NULL 做指针运算（UBSan 构建下才看得出） */
    CuAssertTrue(tc, 0x02CC5D05U == xxh32(NULL, 0, 0));
    CuAssertTrue(tc, xxh32("", 0, 0) == xxh32(NULL, 0, 0));
    CuAssertTrue(tc, 0xEF46DB3751D8E999ULL == xxh64(NULL, 0, 0));
    CuAssertTrue(tc, 0x2D06800538D394C2ULL == xxh3_64(NULL, 0, 0));
    a = xxh3_128(NULL, 0, 0);
    CuAssertTrue(tc, 0x6001C324468D497FULL == a.low && 0x99AA06D3014798D8ULL == a.high);
    xxh32_init(&c32, 0);
    xxh32_update(&c32, NULL, 0);
    CuAssertTrue(tc, 0x02CC5D05U == xxh32_digest(&c32));
    xxh64_init(&c64, 0);
    xxh64_update(&c64, NULL, 0);
    CuAssertTrue(tc, 0xEF46DB3751D8E999ULL == xxh64_digest(&c64));
    xxh3_init(&c3, 0);
    xxh3_update(&c3, NULL, 0);
    CuAssertTrue(tc, 0x2D06800538D394C2ULL == xxh3_digest64(&c3));
    /* 已有数据时再喂 (NULL, 0) 不改变结果 */
    xxh3_update(&c3, buf, 300);
    xxh3_update(&c3, NULL, 0);
    CuAssertTrue(tc, xxh3_64(buf, 300, 0) == xxh3_digest64(&c3));
    /* digest 路径：固定 seed=0，输出大端 canonical 字节 */
    digest_init(&d, DG_XXH64);
    digest_update(&d, buf, XXH_TESTBUF_LENS);
    CuAssertTrue(tc, XXH64_BLOCK_SIZE == digest_final(&d, out));
    pack_integer(want, xxh64(buf, XXH_TESTBUF_LENS, 0), XXH64_BLOCK_SIZE, 0);
    CuAssertTrue(tc, 0 == memcmp(want, out, XXH64_BLOCK_SIZE));
    digest_free(&d);
    digest_init(&d, DG_XXH32);
    digest_update(&d, buf, XXH_TESTBUF_LENS);
    CuAssertTrue(tc, XXH32_BLOCK_SIZE == digest_final(&d, out));
    pack_integer(want, xxh32(buf, XXH_TESTBUF_LENS, 0), XXH32_BLOCK_SIZE, 0);
    CuAssertTrue(tc, 0 == memcmp(want, out, XXH32_BLOCK_SIZE));
    digest_free(&d);
}

void test_crypt(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_base64);
    SUITE_ADD_TEST(suite, test_base64_invalid);
    SUITE_ADD_TEST(suite, test_base64_long);
    SUITE_ADD_TEST(suite, test_crc);
    SUITE_ADD_TEST(suite, test_crc_lengths);
    SUITE_ADD_TEST(suite, test_digest);
    SUITE_ADD_TEST(suite, test_digest_attr_table);
    SUITE_ADD_TEST(suite, test_md2);
    SUITE_ADD_TEST(suite, test_md4);
    SUITE_ADD_TEST(suite, test_md4_update_chunked);
    SUITE_ADD_TEST(suite, test_hmac);
    SUITE_ADD_TEST(suite, test_hmac_variants);
    SUITE_ADD_TEST(suite, test_hmac_free);
    SUITE_ADD_TEST(suite, test_hmac_pbkdf2);
    SUITE_ADD_TEST(suite, test_digest_cipher_free_zeroed);
    SUITE_ADD_TEST(suite, test_urlraw);
    SUITE_ADD_TEST(suite, test_url_encode_charset);
    SUITE_ADD_TEST(suite, test_urlraw_invalid);
    SUITE_ADD_TEST(suite, test_xor);
    SUITE_ADD_TEST(suite, test_cipher);
    SUITE_ADD_TEST(suite, test_cipher_nist_modes);
    SUITE_ADD_TEST(suite, test_cipher_dofinal_batch_kat);
    SUITE_ADD_TEST(suite, test_cipher_padding_zeroed);
    SUITE_ADD_TEST(suite, test_cipher_decrypt_bad_padding);
    SUITE_ADD_TEST(suite, test_cipher_block_reset);
    SUITE_ADD_TEST(suite, test_cipher_init_iv_zeroed);
    SUITE_ADD_TEST(suite, test_cipher_stream_modes);
    SUITE_ADD_TEST(suite, test_cipher_stream_decrypt);
    SUITE_ADD_TEST(suite, test_padding);
    SUITE_ADD_TEST(suite, test_padding_extra);
    SUITE_ADD_TEST(suite, test_md5_nist);
    SUITE_ADD_TEST(suite, test_sha1_nist);
    SUITE_ADD_TEST(suite, test_sha256_nist);
    SUITE_ADD_TEST(suite, test_sha512_nist);
    SUITE_ADD_TEST(suite, test_digest_multiblock);
    SUITE_ADD_TEST(suite, test_xxh32_vectors);
    SUITE_ADD_TEST(suite, test_xxh64_vectors);
    SUITE_ADD_TEST(suite, test_xxh3_64_vectors);
    SUITE_ADD_TEST(suite, test_xxh3_128_vectors);
    SUITE_ADD_TEST(suite, test_xxh_misc);
    SUITE_ADD_TEST(suite, test_xxh_init_garbage);
    SUITE_ADD_TEST(suite, test_aes_direct);
    SUITE_ADD_TEST(suite, test_des_direct);
    SUITE_ADD_TEST(suite, test_scram_handshake);
    SUITE_ADD_TEST(suite, test_scram_rfc_vectors);
    SUITE_ADD_TEST(suite, test_scram_plus);
    SUITE_ADD_TEST(suite, test_scram_plus_requires_cbind);
    SUITE_ADD_TEST(suite, test_scram_gs2_header);
    SUITE_ADD_TEST(suite, test_scram_client_first_attrs);
    SUITE_ADD_TEST(suite, test_scram_gs2_y_handshake);
    SUITE_ADD_TEST(suite, test_scram_cbind_modes);
    SUITE_ADD_TEST(suite, test_scram_server_reject_downgrade);
    SUITE_ADD_TEST(suite, test_scram_failures);
    SUITE_ADD_TEST(suite, test_scram_setters);
    SUITE_ADD_TEST(suite, test_scram_pwd_required);
    SUITE_ADD_TEST(suite, test_scram_embedded_nul);
    SUITE_ADD_TEST(suite, test_scram_server_nonce_required);
    SUITE_ADD_TEST(suite, test_cipher_null_key_iv);
    SUITE_ADD_TEST(suite, test_digest_hmac_final_resets);
    SUITE_ADD_TEST(suite, test_cipher_dofinal_empty_vs_fail);
    SUITE_ADD_TEST(suite, test_scram_empty_user);
    SUITE_ADD_TEST(suite, test_des3_null_key);
}
