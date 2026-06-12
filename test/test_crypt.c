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
static int _scram_handshake(const char *method,
    const char *pwd_cli, const char *pwd_srv,
    const char *cbind_cli, const char *cbind_srv, size_t cbind_len) {
    static const char salt[16] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10
    };
    scram_ctx *cli = scram_init(method, 1);
    scram_ctx *srv = scram_init(method, 0);
    char *cf = NULL, *sf = NULL, *clf = NULL, *svf = NULL;
    int rtn = ERR_FAILED;
    if (!cli || !srv) goto out;

    scram_set_user(cli, "user");
    scram_set_pwd(cli, pwd_cli);
    scram_set_pwd(srv, pwd_srv);
    if (cbind_cli && cbind_len > 0) scram_set_cbind(cli, cbind_cli, cbind_len);
    if (cbind_srv && cbind_len > 0) scram_set_cbind(srv, cbind_srv, cbind_len);
    scram_set_salt(srv, (char *)salt, sizeof(salt));
    scram_set_iter(srv, 4096);

    cf = scram_first_message(cli);
    if (!cf || ERR_OK != scram_parse_first_message(srv, cf, strlen(cf))) goto out;
    FREE(cf); cf = NULL;

    sf = scram_first_message(srv);
    if (!sf || ERR_OK != scram_parse_first_message(cli, sf, strlen(sf))) goto out;
    FREE(sf); sf = NULL;

    clf = scram_final_message(cli);
    if (!clf || ERR_OK != scram_check_final_message(srv, clf, strlen(clf))) goto out;
    FREE(clf); clf = NULL;

    svf = scram_final_message(srv);
    if (!svf || ERR_OK != scram_check_final_message(cli, svf, strlen(svf))) goto out;
    FREE(svf); svf = NULL;

    /* 握手完成后的期望状态：
     * - 客户端已验证服务端签名 → SCRAM_REMOTE_FINAL
     * - 服务端已发送 v= 消息    → SCRAM_LOCAL_FINAL
     *   （服务端在 scram_check_final_message 时达到 SCRAM_REMOTE_FINAL，
     *    随即在 scram_final_message 生成 v= 后转为 SCRAM_LOCAL_FINAL，
     *    不会再收到对端消息，不会进入第二次 REMOTE_FINAL）*/
    rtn = (SCRAM_REMOTE_FINAL == cli->status && SCRAM_LOCAL_FINAL == srv->status)
          ? ERR_OK : ERR_FAILED;
out:
    FREE(cf); FREE(sf); FREE(clf); FREE(svf);
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
        static const char salt[16] = {
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
            0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10
        };
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        scram_ctx *srv = scram_init("SCRAM-SHA-256", 0);
        scram_set_user(cli, "alice");
        scram_set_pwd(cli, "pass");
        scram_set_pwd(srv, "pass");
        scram_set_salt(srv, (char *)salt, sizeof(salt));
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
        scram_set_user(cli, "user");
        scram_set_pwd(cli, "pencil");

        char *first = scram_first_message(cli); // 推进状态至 LOCAL_FIRST
        FREE(first);
        _scram_inject_nonce(cli, "fyko+d2lbbFgONRv9qkxdawL", "user");

        CuAssertIntEquals(tc, ERR_OK,
            scram_parse_first_message(cli, (char *)srv_first, strlen(srv_first)));

        char *cli_final = scram_final_message(cli);
        CuAssertPtrNotNull(tc, cli_final);
        CuAssertStrEquals(tc, exp_cli_final, cli_final);
        FREE(cli_final);

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
        scram_set_user(cli, "user");
        scram_set_pwd(cli, "pencil");

        char *first = scram_first_message(cli);
        FREE(first);
        _scram_inject_nonce(cli, "rOprNGfwEbeRWgbNEkqO", "user");

        CuAssertIntEquals(tc, ERR_OK,
            scram_parse_first_message(cli, (char *)srv_first, strlen(srv_first)));

        char *cli_final = scram_final_message(cli);
        CuAssertPtrNotNull(tc, cli_final);
        CuAssertStrEquals(tc, exp_cli_final, cli_final);
        FREE(cli_final);

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
        scram_set_user(cli, "user");
        scram_set_pwd(cli, "pass");
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

/* 各类失败情形 */
static void test_scram_failures(CuTest *tc) {
    /* 不支持的方法 → NULL */
    CuAssertTrue(tc, NULL == scram_init("SCRAM-MD5", 1));
    CuAssertTrue(tc, NULL == scram_init("", 0));

    /* 密码不匹配 → 服务端拒绝客户端证明 */
    CuAssertTrue(tc, ERR_OK != _scram_handshake(
        "SCRAM-SHA-256", "right", "wrong", NULL, NULL, 0));

    /* 低迭代轮数 → 客户端解析时拒绝 */
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        CuAssertPtrNotNull(tc, cli);
        scram_set_pwd(cli, "pass");
        char *first = scram_first_message(cli);
        CuAssertPtrNotNull(tc, first);
        FREE(first);
        /* 伪造含低迭代轮数的服务端消息，nonce 前缀与客户端一致以绕过 nonce 校验 */
        char fake_srv[256];
        SNPRINTF(fake_srv, sizeof(fake_srv),
            "r=%sFAKESUFFIX,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=100",
            cli->local_nonce);
        CuAssertTrue(tc,
            ERR_OK != scram_parse_first_message(cli, fake_srv, strlen(fake_srv)));
        scram_free(cli);
    }

    /* 服务端签名被篡改 → 客户端拒绝 */
    {
        static const char salt[16] = {
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
            0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10
        };
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        scram_ctx *srv = scram_init("SCRAM-SHA-256", 0);
        scram_set_user(cli, "user");
        scram_set_pwd(cli, "pass");
        scram_set_pwd(srv, "pass");
        scram_set_salt(srv, (char *)salt, sizeof(salt));
        scram_set_iter(srv, 4096);

        char *cf = scram_first_message(cli);
        scram_parse_first_message(srv, cf, strlen(cf)); FREE(cf);
        char *sf = scram_first_message(srv);
        scram_parse_first_message(cli, sf, strlen(sf)); FREE(sf);
        char *clf = scram_final_message(cli);
        scram_check_final_message(srv, clf, strlen(clf)); FREE(clf);

        char *svf = scram_final_message(srv);
        CuAssertPtrNotNull(tc, svf);
        svf[2]++; /* 篡改签名首字节 */
        CuAssertTrue(tc, ERR_OK != scram_check_final_message(cli, svf, strlen(svf)));
        FREE(svf);
        scram_free(cli);
        scram_free(srv);
    }

    /* 状态机：未完成首轮交换就调用 scram_final_message → NULL */
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        scram_set_pwd(cli, "pass");
        /* status=INIT → NULL */
        CuAssertTrue(tc, NULL == scram_final_message(cli));
        char *first = scram_first_message(cli);
        FREE(first);
        /* status=LOCAL_FIRST，未解析服务端消息 → NULL */
        CuAssertTrue(tc, NULL == scram_final_message(cli));
        scram_free(cli);
    }

    /* 非 PLUS 变体调用 scram_set_cbind 应被忽略 */
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        scram_set_cbind(cli, "binddata", 8);
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
        static const char salt[16] = {
            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
            0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10
        };
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        scram_ctx *srv = scram_init("SCRAM-SHA-256", 0);
        scram_set_user(cli, "user,=test");
        scram_set_pwd(cli, "pass");
        scram_set_pwd(srv, "pass");
        scram_set_salt(srv, (char *)salt, sizeof(salt));
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

// scram setter 角色拒绝路径 + scram_set_pwd 超长拒绝 + scram_free(NULL) NULL safety
static void test_scram_setters(CuTest *tc) {
    // scram_free(NULL) 不崩
    scram_free(NULL);

    // scram_set_pwd 长度 = sizeof(scram->pwd) - 1 = 511 字节，刚好通过
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        CuAssertPtrNotNull(tc, cli);
        char pwd511[512];
        memset(pwd511, 'x', 511);
        pwd511[511] = '\0';
        CuAssertIntEquals(tc, ERR_OK, scram_set_pwd(cli, pwd511));
        scram_free(cli);
    }
    // scram_set_pwd 长度 = 512 字节，>= sizeof，被拒绝
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        char pwd512[513];
        memset(pwd512, 'x', 512);
        pwd512[512] = '\0';
        CuAssertIntEquals(tc, ERR_FAILED, scram_set_pwd(cli, pwd512));
        scram_free(cli);
    }
    // scram_set_iter：服务端 iter < SCRAM_MIN_ITER(4096) 自动提升到 4096
    {
        scram_ctx *srv = scram_init("SCRAM-SHA-256", 0);
        scram_set_iter(srv, 100);
        CuAssertIntEquals(tc, 4096, srv->iter);
        scram_set_iter(srv, 10000);
        CuAssertIntEquals(tc, 10000, srv->iter);
        scram_free(srv);
    }
    // scram_set_iter：客户端调用被忽略（iter 保持 0）
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        scram_set_iter(cli, 8192);
        CuAssertIntEquals(tc, 0, cli->iter);
        scram_free(cli);
    }
    // scram_set_salt：客户端调用被忽略
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        char salt[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        scram_set_salt(cli, salt, sizeof(salt));
        CuAssertTrue(tc, NULL == cli->salt);
        CuAssertIntEquals(tc, 0, cli->saltlen);
        scram_free(cli);
    }
    // scram_set_salt：服务端 NULL 或 0 长度被忽略
    {
        scram_ctx *srv = scram_init("SCRAM-SHA-256", 0);
        scram_set_salt(srv, NULL, 8);
        CuAssertTrue(tc, NULL == srv->salt);
        char salt[4] = { 9, 9, 9, 9 };
        scram_set_salt(srv, salt, 0);
        CuAssertTrue(tc, NULL == srv->salt);
        // 正常路径
        scram_set_salt(srv, salt, sizeof(salt));
        CuAssertPtrNotNull(tc, srv->salt);
        CuAssertIntEquals(tc, 4, srv->saltlen);
        // 再次设置覆盖之前
        char salt2[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        scram_set_salt(srv, salt2, sizeof(salt2));
        CuAssertIntEquals(tc, 8, srv->saltlen);
        scram_free(srv);
    }
    // scram_set_user：服务端调用被忽略（user 保持空字符串）
    {
        scram_ctx *srv = scram_init("SCRAM-SHA-256", 0);
        scram_set_user(srv, "should_be_ignored");
        CuAssertTrue(tc, '\0' == srv->user[0]);
        scram_free(srv);
    }
    // scram_set_user：超长用户名截断到 sizeof(user) - 1 (= 63)
    {
        scram_ctx *cli = scram_init("SCRAM-SHA-256", 1);
        char longuser[128];
        memset(longuser, 'A', 127);
        longuser[127] = '\0';
        scram_set_user(cli, longuser);
        CuAssertIntEquals(tc, 63, (int)strlen(cli->user));
        scram_free(cli);
    }
}

/* =======================================================================
 * cipher —— AES / DES 加解密往返验证
 * ======================================================================= */
static void test_cipher(CuTest *tc) {
    const char *key16  = "0123456789abcdef"; /* AES-128 密钥（16 字节）*/
    const char *iv16   = "abcdef0123456789"; /* CBC/CFB/OFB/CTR IV */
    const char *plain  = "Hello, Cipher!!!"; /* 整块明文（16 字节）*/
    const char *plain2 = "short";            /* 非整块明文（5 字节）*/
    char enc_buf[64], dec_buf[64];
    size_t enc_len, dec_len;
    cipher_ctx enc, dec;

    /* ── AES-128 ECB + PKCS7 往返 ── */
    cipher_init(&enc, AES, ECB, key16, 16, 128, 1);
    cipher_padding(&enc, PKCS57);
    cipher_init(&dec, AES, ECB, key16, 16, 128, 0);
    cipher_padding(&dec, PKCS57);

    enc_len = cipher_dofinal(&enc, plain, 16, enc_buf);
    dec_len = cipher_dofinal(&dec, enc_buf, enc_len, dec_buf);
    CuAssertTrue(tc, 16 == (int)dec_len);
    CuAssertTrue(tc, 0 == memcmp(plain, dec_buf, 16));

    /* 非整块数据（5 字节），填充后可正确还原 */
    enc_len = cipher_dofinal(&enc, plain2, strlen(plain2), enc_buf);
    dec_len = cipher_dofinal(&dec, enc_buf, enc_len, dec_buf);
    CuAssertTrue(tc, (int)strlen(plain2) == (int)dec_len);
    CuAssertTrue(tc, 0 == memcmp(plain2, dec_buf, dec_len));

    /* ── AES-128 CBC + PKCS7 往返 ── */
    cipher_init(&enc, AES, CBC, key16, 16, 128, 1);
    cipher_padding(&enc, PKCS57);
    cipher_iv(&enc, iv16, 16);
    cipher_init(&dec, AES, CBC, key16, 16, 128, 0);
    cipher_padding(&dec, PKCS57);
    cipher_iv(&dec, iv16, 16);

    enc_len = cipher_dofinal(&enc, plain, 16, enc_buf);
    dec_len = cipher_dofinal(&dec, enc_buf, enc_len, dec_buf);
    CuAssertTrue(tc, 16 == (int)dec_len);
    CuAssertTrue(tc, 0 == memcmp(plain, dec_buf, 16));

    /* ECB 与 CBC 密文不同（CBC 受 IV 影响） */
    char ecb_enc[64], cbc_enc[64];
    cipher_init(&enc, AES, ECB, key16, 16, 128, 1);
    cipher_padding(&enc, PKCS57);
    cipher_dofinal(&enc, plain, 16, ecb_enc);

    cipher_init(&enc, AES, CBC, key16, 16, 128, 1);
    cipher_padding(&enc, PKCS57);
    cipher_iv(&enc, iv16, 16);
    cipher_dofinal(&enc, plain, 16, cbc_enc);
    CuAssertTrue(tc, 0 != memcmp(ecb_enc, cbc_enc, 16));

    /* ── DES ECB + PKCS7 往返（DES 密钥 8 字节，分组 8 字节）── */
    const char *des_key = "8bytekey";
    cipher_init(&enc, DES, ECB, des_key, 8, 0, 1);
    cipher_padding(&enc, PKCS57);
    cipher_init(&dec, DES, ECB, des_key, 8, 0, 0);
    cipher_padding(&dec, PKCS57);

    enc_len = cipher_dofinal(&enc, plain2, strlen(plain2), enc_buf);
    dec_len = cipher_dofinal(&dec, enc_buf, enc_len, dec_buf);
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

    size_t enc_len = cipher_dofinal(&enc, plain, 16, enc_buf);
    CuAssertTrue(tc, 32 == (int)enc_len);                 /* 16 数据 + 16 填充块 */

    memset(dec_buf, 0x5a, sizeof(dec_buf));
    size_t dec_len = cipher_dofinal(&dec, enc_buf, enc_len, dec_buf);
    CuAssertTrue(tc, 16 == (int)dec_len);
    CuAssertTrue(tc, 0 == memcmp(plain, dec_buf, 16));
    /* 剥离的 16 字节 padding(原值 0x10)修复后应已清零 */
    char zero[16] = { 0 };
    CuAssertTrue(tc, 0 == memcmp(dec_buf + dec_len, zero, enc_len - dec_len));

    cipher_free(&enc);
    cipher_free(&dec);
}

/* =======================================================================
 * HMAC —— SHA-1 / SHA-512 已知测试向量
 * ======================================================================= */
static void test_hmac_variants(CuTest *tc) {
    char hash[DG_BLOCK_SIZE];
    char hex[DG_BLOCK_SIZE * 2 + 1];
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
    _to_hex(hash, hlen, hex);
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
    _to_hex(hash, hlen, hex);
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
    char hex[MD2_BLOCK_SIZE * 2 + 1];

    /* RFC 1319 Appendix A.5 测试向量 */
    /* MD2("") = 8350e5a3e24c153df2275c9f80692773 */
    md2_init(&ctx);
    md2_update(&ctx, "", 0);
    md2_final(&ctx, hash);
    _to_hex(hash, MD2_BLOCK_SIZE, hex);
    CuAssertStrEquals(tc, "8350e5a3e24c153df2275c9f80692773", hex);

    /* MD2("a") = 32ec01ec4a6dac72c0ab96fb34c0b5d1 */
    md2_init(&ctx);
    md2_update(&ctx, "a", 1);
    md2_final(&ctx, hash);
    _to_hex(hash, MD2_BLOCK_SIZE, hex);
    CuAssertStrEquals(tc, "32ec01ec4a6dac72c0ab96fb34c0b5d1", hex);

    /* MD2("abc") = da853b0d3f88d99b30283a69e6ded6bb */
    md2_init(&ctx);
    md2_update(&ctx, "abc", 3);
    md2_final(&ctx, hash);
    _to_hex(hash, MD2_BLOCK_SIZE, hex);
    CuAssertStrEquals(tc, "da853b0d3f88d99b30283a69e6ded6bb", hex);

    /* MD2("message digest") = ab4f496bfb2a530b219ff33031fe06b0 */
    md2_init(&ctx);
    md2_update(&ctx, "message digest", 14);
    md2_final(&ctx, hash);
    _to_hex(hash, MD2_BLOCK_SIZE, hex);
    CuAssertStrEquals(tc, "ab4f496bfb2a530b219ff33031fe06b0", hex);

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
    char hex[MD4_BLOCK_SIZE * 2 + 1];

    /* MD4("") = 31d6cfe0d16ae931b73c59d7e0c089c0 */
    md4_init(&ctx);
    md4_update(&ctx, "", 0);
    md4_final(&ctx, hash);
    _to_hex(hash, MD4_BLOCK_SIZE, hex);
    CuAssertStrEquals(tc, "31d6cfe0d16ae931b73c59d7e0c089c0", hex);

    /* MD4("a") = bde52cb31de33e46245e05fbdbd6fb24 */
    md4_init(&ctx);
    md4_update(&ctx, "a", 1);
    md4_final(&ctx, hash);
    _to_hex(hash, MD4_BLOCK_SIZE, hex);
    CuAssertStrEquals(tc, "bde52cb31de33e46245e05fbdbd6fb24", hex);

    /* MD4("abc") = a448017aaf21d8525fc10ae87aa6729d */
    md4_init(&ctx);
    md4_update(&ctx, "abc", 3);
    md4_final(&ctx, hash);
    _to_hex(hash, MD4_BLOCK_SIZE, hex);
    CuAssertStrEquals(tc, "a448017aaf21d8525fc10ae87aa6729d", hex);

    /* MD4("message digest") = d9130a8164549fe818874806e1c7014b */
    md4_init(&ctx);
    md4_update(&ctx, "message digest", 14);
    md4_final(&ctx, hash);
    _to_hex(hash, MD4_BLOCK_SIZE, hex);
    CuAssertStrEquals(tc, "d9130a8164549fe818874806e1c7014b", hex);

    /* MD4("abcdefghijklmnopqrstuvwxyz") = d79e1c308aa5bbcdeea8ed63df412da9 */
    md4_init(&ctx);
    md4_update(&ctx, "abcdefghijklmnopqrstuvwxyz", 26);
    md4_final(&ctx, hash);
    _to_hex(hash, MD4_BLOCK_SIZE, hex);
    CuAssertStrEquals(tc, "d79e1c308aa5bbcdeea8ed63df412da9", hex);

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

// md5 直调 RFC 1321 标准测试向量集
static void test_md5_nist(CuTest *tc) {
    md5_ctx ctx;
    char hash[MD5_BLOCK_SIZE];
    char hex[MD5_BLOCK_SIZE * 2 + 1];
    struct { const char *in; const char *expect; } cases[] = {
        { "",                                                                "d41d8cd98f00b204e9800998ecf8427e" },
        { "a",                                                               "0cc175b9c0f1b6a831c399e269772661" },
        { "abc",                                                             "900150983cd24fb0d6963f7d28e17f72" },
        { "message digest",                                                  "f96b697d7cb7938d525a2f31aaf161d0" },
        { "abcdefghijklmnopqrstuvwxyz",                                      "c3fcd3d76192e4007dfb496cca67e13b" },
        { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",  "d174ab98d277d9f5a5611c2c9f419d9f" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        md5_init(&ctx);
        md5_update(&ctx, cases[i].in, strlen(cases[i].in));
        md5_final(&ctx, hash);
        _to_hex(hash, MD5_BLOCK_SIZE, hex);
        CuAssertStrEquals(tc, cases[i].expect, hex);
    }
    // 跨 block（>64 字节）正确性：800 个 'a' 触发多 block + final padding
    char buf[800];
    memset(buf, 'a', sizeof(buf));
    md5_init(&ctx);
    md5_update(&ctx, buf, sizeof(buf));
    md5_final(&ctx, hash);
    _to_hex(hash, MD5_BLOCK_SIZE, hex);
    char hash2[MD5_BLOCK_SIZE];
    char hex2[MD5_BLOCK_SIZE * 2 + 1];
    // 分段 update 与整体 update 结果一致
    md5_init(&ctx);
    md5_update(&ctx, buf, 400);
    md5_update(&ctx, buf + 400, 400);
    md5_final(&ctx, hash2);
    _to_hex(hash2, MD5_BLOCK_SIZE, hex2);
    CuAssertStrEquals(tc, hex, hex2);
}

// sha1 直调 FIPS 180-1 / RFC 3174 标准测试向量集
static void test_sha1_nist(CuTest *tc) {
    sha1_ctx ctx;
    char hash[SHA1_BLOCK_SIZE];
    char hex[SHA1_BLOCK_SIZE * 2 + 1];
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
        _to_hex(hash, SHA1_BLOCK_SIZE, hex);
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
    _to_hex(hash, SHA1_BLOCK_SIZE, hex);
    CuAssertStrEquals(tc, "34aa973cd4c4daa4f61eeb2bdbad27316534016f", hex);
}

// sha256 直调 FIPS 180-2 标准测试向量集
static void test_sha256_nist(CuTest *tc) {
    sha256_ctx ctx;
    char hash[SHA256_BLOCK_SIZE];
    char hex[SHA256_BLOCK_SIZE * 2 + 1];
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
        _to_hex(hash, SHA256_BLOCK_SIZE, hex);
        CuAssertStrEquals(tc, cases[i].expect, hex);
    }
    // 分段 update（单字节流） vs 整体 update 等价
    const char *msg = "The quick brown fox jumps over the lazy dog";
    size_t mlen = strlen(msg);
    sha256_init(&ctx);
    sha256_update(&ctx, msg, mlen);
    sha256_final(&ctx, hash);
    char hex_whole[SHA256_BLOCK_SIZE * 2 + 1];
    _to_hex(hash, SHA256_BLOCK_SIZE, hex_whole);
    sha256_ctx ctx2;
    sha256_init(&ctx2);
    for (size_t i = 0; i < mlen; i++) {
        sha256_update(&ctx2, msg + i, 1);
    }
    sha256_final(&ctx2, hash);
    char hex_byte[SHA256_BLOCK_SIZE * 2 + 1];
    _to_hex(hash, SHA256_BLOCK_SIZE, hex_byte);
    CuAssertStrEquals(tc, hex_whole, hex_byte);
    // 已知值：SHA256("The quick brown fox jumps over the lazy dog")
    CuAssertStrEquals(tc, "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592", hex_whole);
}

// sha512 直调 FIPS 180-2 标准测试向量集
static void test_sha512_nist(CuTest *tc) {
    sha512_ctx ctx;
    char hash[SHA512_BLOCK_SIZE];
    char hex[SHA512_BLOCK_SIZE * 2 + 1];
    sha512_init(&ctx);
    sha512_update(&ctx, "", 0);
    sha512_final(&ctx, hash);
    _to_hex(hash, SHA512_BLOCK_SIZE, hex);
    CuAssertStrEquals(tc,
        "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
        hex);
    sha512_init(&ctx);
    sha512_update(&ctx, "abc", 3);
    sha512_final(&ctx, hash);
    _to_hex(hash, SHA512_BLOCK_SIZE, hex);
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
    _to_hex(hash, SHA512_BLOCK_SIZE, hex);
    CuAssertStrEquals(tc,
        "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
        "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909",
        hex);
}

// 将 hex 字符串转为字节数组，长度必须是偶数
static void _hex_to_bytes(const char *hex, uint8_t *out, size_t outlen) {
    for (size_t i = 0; i < outlen; i++) {
        char c1 = hex[i * 2];
        char c2 = hex[i * 2 + 1];
        uint8_t v1 = (c1 >= 'a') ? (uint8_t)(c1 - 'a' + 10) : ((c1 >= 'A') ? (uint8_t)(c1 - 'A' + 10) : (uint8_t)(c1 - '0'));
        uint8_t v2 = (c2 >= 'a') ? (uint8_t)(c2 - 'a' + 10) : ((c2 >= 'A') ? (uint8_t)(c2 - 'A' + 10) : (uint8_t)(c2 - '0'));
        out[i] = (uint8_t)((v1 << 4) | v2);
    }
}

// aes_init / aes_crypt 直调，NIST FIPS-197 Appendix A/B 标准向量 + 128/192/256 keybits
static void test_aes_direct(CuTest *tc) {
    aes_ctx aes;
    char hex[AES_BLOCK_SIZE * 2 + 1];

    // ── AES-128 ECB（NIST SP 800-38A F.1.1）──
    {
        uint8_t key[16], pt[16];
        _hex_to_bytes("2b7e151628aed2a6abf7158809cf4f3c", key, 16);
        _hex_to_bytes("6bc1bee22e409f96e93d7e117393172a", pt, 16);
        aes_init(&aes, (char *)key, 16, 128, 1);
        char *ct = aes_crypt(&aes, pt);
        _to_hex(ct, AES_BLOCK_SIZE, hex);
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
        _to_hex(ct, AES_BLOCK_SIZE, hex);
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
        _to_hex(ct, AES_BLOCK_SIZE, hex);
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
        char hex[DES_BLOCK_SIZE * 2 + 1];
        _to_hex(ct, DES_BLOCK_SIZE, hex);
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
}

/* =======================================================================
 * padding —— 各种填充模式
 * ======================================================================= */
static void test_padding(CuTest *tc) {
    uint8_t out[16];

    /* ── ZeroPadding：填充字节为 0 ── */
    ZERO(out, sizeof(out));
    _padding_data(ZeroPadding, "abc", 3, out, 8);
    CuAssertTrue(tc, 0 == memcmp(out, "abc", 3));
    CuAssertTrue(tc, 0 == out[3] && 0 == out[4] && 0 == out[5]
                    && 0 == out[6] && 0 == out[7]);

    /* ── PKCS57：填充字节值等于填充长度 ── */
    ZERO(out, sizeof(out));
    _padding_data(PKCS57, "abc", 3, out, 8);
    CuAssertTrue(tc, 0 == memcmp(out, "abc", 3));
    /* 剩余 5 字节均为 5 */
    for (int i = 3; i < 8; i++) {
        CuAssertTrue(tc, 5 == out[i]);
    }

    /* ── ANSIX923：前置零 + 末尾填充长度 ── */
    ZERO(out, sizeof(out));
    _padding_data(ANSIX923, "ab", 2, out, 8);
    CuAssertTrue(tc, 0 == memcmp(out, "ab", 2));
    /* 中间 5 字节零 */
    for (int i = 2; i < 7; i++) {
        CuAssertTrue(tc, 0 == out[i]);
    }
    /* 末尾字节为填充长度 6 */
    CuAssertTrue(tc, 6 == out[7]);

    /* ── ISO10126：随机字节 + 末尾填充长度 ── */
    ZERO(out, sizeof(out));
    _padding_data(ISO10126, "ab", 2, out, 8);
    CuAssertTrue(tc, 0 == memcmp(out, "ab", 2));
    /* 末尾字节为填充长度 */
    CuAssertTrue(tc, 6 == out[7]);

    /* ── 输入长度等于要求长度：原样拷贝，不填充 ── */
    uint8_t out2[8];
    ZERO(out2, sizeof(out2));
    _padding_data(PKCS57, "12345678", 8, out2, 8);
    CuAssertTrue(tc, 0 == memcmp(out2, "12345678", 8));

    /* ── 输入长度 > 要求长度：函数提前返回，不写入 output ── */
    uint8_t out3[4];
    ZERO(out3, sizeof(out3));
    _padding_data(PKCS57, "abcdef", 6, out3, 4);
    /* output 保持初始零状态（dlens > reqlens 时早返回）*/
    for (int i = 0; i < 4; i++) {
        CuAssertTrue(tc, 0 == out3[i]);
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
    _padding_data(PKCS57, "hello", 5, out16, 16);
    CuAssertTrue(tc, 0 == memcmp(out16, "hello", 5));
    // 剩 11 字节都应该是 11 (0x0B)
    for (int i = 5; i < 16; i++) {
        CuAssertIntEquals(tc, 11, out16[i]);
    }

    // ── PKCS57 dlens=0：整 block 都填充 reqlens 字节 ──
    uint8_t pblock[16];
    ZERO(pblock, sizeof(pblock));
    _padding_data(PKCS57, NULL, 0, pblock, 16);
    for (int i = 0; i < 16; i++) {
        CuAssertIntEquals(tc, 16, pblock[i]);
    }

    // ── ZeroPadding 在 32-byte block ──
    uint8_t out32[32];
    memset(out32, 0xff, sizeof(out32));
    _padding_data(ZeroPadding, "abcd", 4, out32, 32);
    CuAssertTrue(tc, 0 == memcmp(out32, "abcd", 4));
    for (int i = 4; i < 32; i++) {
        CuAssertIntEquals(tc, 0, out32[i]);
    }

    // ── ANSIX923 在 16-byte block 边界 ──
    uint8_t ansi[16];
    memset(ansi, 0xff, sizeof(ansi));
    _padding_data(ANSIX923, "ABC", 3, ansi, 16);
    CuAssertTrue(tc, 0 == memcmp(ansi, "ABC", 3));
    // 中间 12 字节为 0
    for (int i = 3; i < 15; i++) {
        CuAssertIntEquals(tc, 0, ansi[i]);
    }
    // 最后字节为填充长度 13
    CuAssertIntEquals(tc, 13, ansi[15]);

    // ── ISO10126：末尾字节为 padlen，前面字节随机（不验证值，仅验证末尾）──
    uint8_t iso[16];
    _padding_data(ISO10126, "X", 1, iso, 16);
    CuAssertIntEquals(tc, 'X', iso[0]);
    CuAssertIntEquals(tc, 15, iso[15]);

    // ── NoPadding：默认分支不写 padding 字节（保持原状）──
    uint8_t nopad[16];
    memset(nopad, 0xab, sizeof(nopad));
    _padding_data(NoPadding, "xy", 2, nopad, 16);
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
    for (int i = (int)rlen; i < 16; i++) CuAssertIntEquals(tc, 0, k16[i]);
    for (int i = (int)rlen; i < 24; i++) CuAssertIntEquals(tc, 0, k24[i]);
    for (int i = (int)rlen; i < 32; i++) CuAssertIntEquals(tc, 0, k32[i]);
}

/* =======================================================================
 * cipher_block / cipher_reset —— 分组级 API 与 IV 重置
 * cipher_block 直接对单个分组加解密；
 * cipher_reset 将 cur_iv 重置为初始 iv，CTR/CFB/OFB 流模式下复用同一 ctx 必须先 reset
 * ======================================================================= */
static void test_cipher_block_reset(CuTest *tc) {
    const char *key16 = "0123456789abcdef";
    const char *iv16  = "abcdef0123456789";
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
 * cipher 流模式完整 round-trip —— CFB / OFB / CTR
 * 现有 test_cipher 仅覆盖 ECB/CBC + PKCS7 整套 dofinal 流程；
 * test_cipher_block_reset 仅触及流模式单分组 cipher_block + reset。
 * 本测试覆盖 cipher_init → cipher_iv → cipher_dofinal 完整流程，跨多种明文长度
 * （非整 block / 整 2 block / 跨 2+1 block）验证流模式密文长度等于明文长度且可还原。
 * ======================================================================= */
static void test_cipher_stream_modes(CuTest *tc) {
    const char *key16 = "0123456789abcdef";
    const char *iv16  = "abcdef0123456789";
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

            elen = cipher_dofinal(&enc, plain, plen, enc_buf);
            // 流模式：密文长度严格等于明文长度（无填充）
            CuAssertTrue(tc, plen == elen);
            // 密文与明文不同
            CuAssertTrue(tc, 0 != memcmp(plain, enc_buf, plen));

            dlen = cipher_dofinal(&dec, enc_buf, elen, dec_buf);
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

/* =======================================================================
 * base64 非法输入：单字符残组(有效字符 %4==1) / 内嵌非法字符 / '=' 后非 = 字符 返回 0
 * ======================================================================= */
static void test_base64_invalid(CuTest *tc) {
    char out[64];

    /* 空输入、有效字符数 %4==1（单字符残组无法构成字节）返回 0 */
    CuAssertTrue(tc, 0 == bs64_decode("",  0, out));
    CuAssertTrue(tc, 0 == bs64_decode("A", 1, out));

    /* 含非法字符（< '+' 或 > 'z'） */
    CuAssertTrue(tc, 0 == bs64_decode("A!BC", 4, out));   /* '!' < '+' */
    CuAssertTrue(tc, 0 == bs64_decode("AB{C", 4, out));   /* '{' > 'z' */
    CuAssertTrue(tc, 0 == bs64_decode("AB C", 4, out));   /* ' ' < '+' */

    /* '+' 至 'z' 区间内的非 base64 字符（查表 -1） */
    CuAssertTrue(tc, 0 == bs64_decode("A,BC", 4, out));   /* ',' 在表中为 -1 */
    CuAssertTrue(tc, 0 == bs64_decode("AB.C", 4, out));   /* '.' 在表中为 -1 */

    /* '=' 后接非 '='/CR/LF 字符：伪造截断防御 */
    CuAssertTrue(tc, 0 == bs64_decode("AB=A", 4, out));
    CuAssertTrue(tc, 0 == bs64_decode("A=BC", 4, out));

    /* 无填充 base64（RFC 4648 §3.2）：单字符残组（%4==1）非法 → 0，其余正常解码 */
    CuAssertTrue(tc, 0 == bs64_decode("TWFuT", 5, out));       /* 有效字符 % 4 == 1，非法 */

    size_t nlen = bs64_decode("TWFuTW", 6, out);              /* % 4 == 2，原 ASan 越界点 */
    CuAssertTrue(tc, 4 == nlen && 0 == memcmp(out, "ManM", 4));
    nlen = bs64_decode("TWFuTWF", 7, out);                    /* % 4 == 3，原 ASan 越界点 */
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
    CuAssertStrEquals(tc, "a b c", buf);
}

/* ======================================================================= */

void test_crypt(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_base64);
    SUITE_ADD_TEST(suite, test_base64_invalid);
    SUITE_ADD_TEST(suite, test_crc);
    SUITE_ADD_TEST(suite, test_digest);
    SUITE_ADD_TEST(suite, test_md2);
    SUITE_ADD_TEST(suite, test_md4);
    SUITE_ADD_TEST(suite, test_hmac);
    SUITE_ADD_TEST(suite, test_hmac_variants);
    SUITE_ADD_TEST(suite, test_hmac_free);
    SUITE_ADD_TEST(suite, test_urlraw);
    SUITE_ADD_TEST(suite, test_urlraw_invalid);
    SUITE_ADD_TEST(suite, test_xor);
    SUITE_ADD_TEST(suite, test_cipher);
    SUITE_ADD_TEST(suite, test_cipher_padding_zeroed);
    SUITE_ADD_TEST(suite, test_cipher_block_reset);
    SUITE_ADD_TEST(suite, test_cipher_stream_modes);
    SUITE_ADD_TEST(suite, test_padding);
    SUITE_ADD_TEST(suite, test_padding_extra);
    SUITE_ADD_TEST(suite, test_md5_nist);
    SUITE_ADD_TEST(suite, test_sha1_nist);
    SUITE_ADD_TEST(suite, test_sha256_nist);
    SUITE_ADD_TEST(suite, test_sha512_nist);
    SUITE_ADD_TEST(suite, test_aes_direct);
    SUITE_ADD_TEST(suite, test_des_direct);
    SUITE_ADD_TEST(suite, test_scram_handshake);
    SUITE_ADD_TEST(suite, test_scram_rfc_vectors);
    SUITE_ADD_TEST(suite, test_scram_plus);
    SUITE_ADD_TEST(suite, test_scram_failures);
    SUITE_ADD_TEST(suite, test_scram_setters);
}
