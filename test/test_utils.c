#include "test_utils.h"
#include "lib.h"

/* =======================================================================
 * pack / unpack —— 整数、浮点数的字节序读写
 * ======================================================================= */
static void test_pack_unpack(CuTest *tc) {
    char buf[8];

    /* int16：小端写入，小端读回（有符号）*/
    int16_t i16 = -6534;
    pack_integer(buf, (uint64_t)(int64_t)i16, 2, 1);
    CuAssertTrue(tc, i16 == (int16_t)unpack_integer(buf, 2, 1, 1));

    /* int16：大端写入，大端读回 */
    pack_integer(buf, (uint64_t)(int64_t)i16, 2, 0);
    CuAssertTrue(tc, i16 == (int16_t)unpack_integer(buf, 2, 0, 1));

    /* int32：小端往返 */
    int32_t i32 = -1234567;
    pack_integer(buf, (uint64_t)(int64_t)i32, 4, 1);
    CuAssertTrue(tc, i32 == (int32_t)unpack_integer(buf, 4, 1, 1));

    /* uint32：大端往返 */
    uint32_t u32 = 0xDEADBEEF;
    pack_integer(buf, u32, 4, 0);
    CuAssertTrue(tc, u32 == (uint32_t)unpack_integer(buf, 4, 0, 0));

    /* float 往返 */
    float f = -123456.789f;
    pack_float(buf, f, 1);
    float f2 = unpack_float(buf, 1);
    CuAssertTrue(tc, (f2 - f) < 0.001f && (f - f2) < 0.001f);

    /* double 往返 */
    double d = 987654321.123456;
    pack_double(buf, d, 1);
    double d2 = unpack_double(buf, 1);
    CuAssertTrue(tc, (d2 - d) < 0.000001 && (d - d2) < 0.000001);

    /* 网络字节序宏（64 位）*/
    uint64_t v64 = 0x0102030405060708ULL;
    uint64_t net = htonll(v64);
    CuAssertTrue(tc, v64 == ntohll(net));
}

/* =======================================================================
 * binary —— 连续内存流式读写
 * ======================================================================= */
static void test_binary(CuTest *tc) {
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 64); /* 动态分配，初始不设缓冲 */

    /* 写入各种类型 */
    int8_t  i8  = -120;      binary_set_int8(&bw, i8);
    uint8_t u8  = 200;       binary_set_uint8(&bw, u8);
    int16_t i16 = -30000;    binary_set_integer(&bw, i16, 2, 1);
    uint16_t u16 = 60000;    binary_set_uinteger(&bw, u16, 2, 1);
    int32_t i32 = -1000000;  binary_set_integer(&bw, i32, 4, 1);
    uint32_t u32 = 3000000;  binary_set_uinteger(&bw, u32, 4, 0); /* 大端 */
    int64_t i64 = -9876543210LL; binary_set_integer(&bw, i64, 8, 1);
    float   fv  = -3.14159f; binary_set_float(&bw, fv, 1);
    double  dv  = 2.718281828; binary_set_double(&bw, dv, 1);
    binary_set_fill(&bw, 0xAB, 4);   /* 填充 4 字节 0xAB */
    binary_set_skip(&bw, 2);         /* 跳过 2 字节（写入 0）*/
    const char *str = "hello";       binary_set_string(&bw, str, 0);   /* 含 \0 */
    const char *bin = "world";       binary_set_string(&bw, bin, 5);   /* 不含 \0 */

    /* 读取并逐一验证 */
    binary_ctx br;
    binary_init(&br, bw.data, bw.offset, 0);

    CuAssertTrue(tc, i8  == binary_get_int8(&br));
    CuAssertTrue(tc, u8  == binary_get_uint8(&br));
    CuAssertTrue(tc, i16 == (int16_t)binary_get_integer(&br, 2, 1));
    CuAssertTrue(tc, u16 == (uint16_t)binary_get_uinteger(&br, 2, 1));
    CuAssertTrue(tc, i32 == (int32_t)binary_get_integer(&br, 4, 1));
    CuAssertTrue(tc, u32 == (uint32_t)binary_get_uinteger(&br, 4, 0));
    CuAssertTrue(tc, i64 == binary_get_integer(&br, 8, 1));

    float  fv2 = binary_get_float(&br, 1);
    CuAssertTrue(tc, (fv2 - fv) < 0.0001f && (fv - fv2) < 0.0001f);
    double dv2 = binary_get_double(&br, 1);
    CuAssertTrue(tc, (dv2 - dv) < 0.000001 && (dv - dv2) < 0.000001);

    /* 跳过填充和保留字节 */
    binary_get_skip(&br, 4 + 2);

    /* 字符串 */
    const char *rs = binary_get_string(&br, 0);
    CuAssertStrEquals(tc, str, rs);
    const char *rb = binary_get_string(&br, 5);
    CuAssertTrue(tc, 0 == memcmp(bin, rb, 5));

    /* 读写游标对齐，已全部消费 */
    CuAssertTrue(tc, br.offset == br.size);

    FREE(bw.data);
}

/* =======================================================================
 * buffer —— 分散内存读写
 * ======================================================================= */
static void test_buffer(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);
    CuAssertTrue(tc, 0 == buffer_size(&buf));

    /* 追加数据 */
    const char *s1 = "Hello";
    const char *s2 = ", World!";
    CuAssertTrue(tc, ERR_OK == buffer_append(&buf, (void *)s1, strlen(s1)));
    CuAssertTrue(tc, ERR_OK == buffer_append(&buf, (void *)s2, strlen(s2)));
    CuAssertTrue(tc, 13 == buffer_size(&buf));

    /* copyout：不删除数据 */
    char out[32] = {0};
    size_t nr = buffer_copyout(&buf, 0, out, 13);
    CuAssertTrue(tc, 13 == nr);
    CuAssertTrue(tc, 0 == memcmp("Hello, World!", out, 13));
    CuAssertTrue(tc, 13 == buffer_size(&buf)); /* 数据仍在 */

    /* copyout：偏移读取 */
    memset(out, 0, sizeof(out));
    nr = buffer_copyout(&buf, 7, out, 6);
    CuAssertTrue(tc, 6 == nr);
    CuAssertTrue(tc, 0 == memcmp("World!", out, 6));

    /* at：指定位置字节 */
    CuAssertTrue(tc, 'H' == buffer_at(&buf, 0));
    CuAssertTrue(tc, '!' == buffer_at(&buf, 12));

    /* search：查找子串 */
    int pos = buffer_search(&buf, 0, 0, 0, ", ", 2);
    CuAssertTrue(tc, 5 == pos);

    /* drain：删除头部数据 */
    size_t nd = buffer_drain(&buf, 5);
    CuAssertTrue(tc, 5 == nd);
    CuAssertTrue(tc, 8 == buffer_size(&buf));
    memset(out, 0, sizeof(out));
    buffer_copyout(&buf, 0, out, 8);
    CuAssertTrue(tc, 0 == memcmp(", World!", out, 8));

    /* remove：读取并删除 */
    memset(out, 0, sizeof(out));
    nr = buffer_remove(&buf, out, 8);
    CuAssertTrue(tc, 8 == nr);
    CuAssertTrue(tc, 0 == memcmp(", World!", out, 8));
    CuAssertTrue(tc, 0 == buffer_size(&buf));

    /* 大量数据追加，触发多节点 */
    char big[8192];
    memset(big, 'A', sizeof(big));
    buffer_append(&buf, big, sizeof(big));
    CuAssertTrue(tc, sizeof(big) == buffer_size(&buf));
    buffer_drain(&buf, sizeof(big));
    CuAssertTrue(tc, 0 == buffer_size(&buf));

    buffer_free(&buf);
}

/* =======================================================================
 * sfid —— 雪花 ID
 * ======================================================================= */
static void test_sfid(CuTest *tc) {
    sfid_ctx ctx;
    sfid_ctx *p = sfid_init(&ctx, 1, 0, 0, 0); /* 机器ID=1，其余默认 */
    CuAssertPtrNotNull(tc, p);

    /* 连续生成的 ID 单调递增 */
    uint64_t prev = sfid_id(&ctx);
    for (int i = 0; i < 100; i++) {
        uint64_t cur = sfid_id(&ctx);
        CuAssertTrue(tc, cur > prev);
        prev = cur;
    }

    /* decode 还原机器ID */
    uint64_t ts;
    int32_t  mid, seq;
    sfid_decode(&ctx, prev, &ts, &mid, &seq);
    CuAssertTrue(tc, 1 == mid);
    CuAssertTrue(tc, ts > 0);
}

/* =======================================================================
 * hash_ring —— 一致性哈希
 * ======================================================================= */
static void test_hash_ring(CuTest *tc) {
    hash_ring_ctx ring;
    hash_ring_init(&ring);

    /* 添加 3 个节点，每节点 150 个虚拟节点 */
    const char *nodes[] = { "node1", "node2", "node3" };
    for (int i = 0; i < 3; i++) {
        CuAssertTrue(tc, ERR_OK == hash_ring_add(&ring,
                     (void *)nodes[i], strlen(nodes[i]), 150));
    }
    CuAssertTrue(tc, 3 == ring.nnodes);
    CuAssertTrue(tc, 450 == ring.nitems);

    /* 查找：相同 key 路由到相同节点 */
    const char *key = "user:12345";
    hash_ring_node *n1 = hash_ring_find(&ring, (void *)key, strlen(key));
    hash_ring_node *n2 = hash_ring_find(&ring, (void *)key, strlen(key));
    CuAssertPtrNotNull(tc, n1);
    CuAssertTrue(tc, n1 == n2);

    /* 移除一个节点后，查找结果仍有效（路由到其余节点）*/
    hash_ring_remove(&ring, (void *)nodes[0], strlen(nodes[0]));
    CuAssertTrue(tc, 2 == ring.nnodes);
    CuAssertTrue(tc, 300 == ring.nitems);
    hash_ring_node *n3 = hash_ring_find(&ring, (void *)key, strlen(key));
    CuAssertPtrNotNull(tc, n3);

    /* 批量添加（nosort）后统一排序 */
    hash_ring_add_nosort(&ring, (void *)"node4", 5, 50);
    hash_ring_add_nosort(&ring, (void *)"node5", 5, 50);
    hash_ring_sort(&ring);
    CuAssertTrue(tc, 4 == ring.nnodes);
    hash_ring_node *n4 = hash_ring_find(&ring, (void *)key, strlen(key));
    CuAssertPtrNotNull(tc, n4);

    hash_ring_free(&ring);
}

/* =======================================================================
 * netaddr —— IP 地址辅助工具
 * ======================================================================= */
static void test_netaddr(CuTest *tc) {
    /* IPv4 检测 */
    CuAssertTrue(tc, ERR_OK == is_ipv4("127.0.0.1"));
    CuAssertTrue(tc, ERR_OK == is_ipv4("192.168.1.100"));
    CuAssertTrue(tc, ERR_OK != is_ipv4("::1"));
    CuAssertTrue(tc, ERR_OK != is_ipv4("not_an_ip"));

    /* IPv6 检测 */
    CuAssertTrue(tc, ERR_OK == is_ipv6("::1"));
    CuAssertTrue(tc, ERR_OK == is_ipv6("fe80::1"));
    CuAssertTrue(tc, ERR_OK != is_ipv6("127.0.0.1"));

    /* is_ipaddr：IPv4 和 IPv6 均匹配 */
    CuAssertTrue(tc, ERR_OK == is_ipaddr("127.0.0.1"));
    CuAssertTrue(tc, ERR_OK == is_ipaddr("::1"));
    CuAssertTrue(tc, ERR_OK != is_ipaddr("example.com"));

    /* netaddr_set / netaddr_empty */
    netaddr_ctx addr;
    CuAssertTrue(tc, ERR_OK == netaddr_set(&addr, "127.0.0.1", 8080));
    /* AF 应为 IPv4 */
    CuAssertTrue(tc, AF_INET == addr.addr.sa_family);

    netaddr_empty(&addr);
    CuAssertTrue(tc, 0 == addr.addr.sa_family);

    /* IPv6 地址 */
    CuAssertTrue(tc, ERR_OK == netaddr_set(&addr, "::1", 9090));
    CuAssertTrue(tc, AF_INET6 == addr.addr.sa_family);
}

/* ======================================================================= */

void test_utils(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_pack_unpack);
    SUITE_ADD_TEST(suite, test_binary);
    SUITE_ADD_TEST(suite, test_buffer);
    SUITE_ADD_TEST(suite, test_sfid);
    SUITE_ADD_TEST(suite, test_hash_ring);
    SUITE_ADD_TEST(suite, test_netaddr);
}
