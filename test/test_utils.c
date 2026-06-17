#include "test_utils.h"
#include "lib.h"
#include "utils/strptime.h"
#include "utils/pool.h"

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

    /* size<=0 边界：0 字节解包恒为 0，避免 1<<(size*8-1) 移位 UB */
    CuAssertTrue(tc, 0 == unpack_integer(buf, 0, 1, 1));
    CuAssertTrue(tc, 0 == unpack_integer(buf, 0, 0, 0));
    CuAssertTrue(tc, 0 == unpack_integer(buf, -1, 0, 1));
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
    const char *str = "hello";       binary_set_string(&bw, str);   /* 含 \0 */
    const char *bin = "world";       binary_set_binary(&bw, bin, 5);   /* 不含 \0 */

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
    const char *rs = binary_get_string(&br);
    CuAssertStrEquals(tc, str, rs);
    const char *rb = binary_get_binary(&br, 5);
    CuAssertTrue(tc, 0 == memcmp(bin, rb, 5));

    /* 读写游标对齐，已全部消费 */
    CuAssertTrue(tc, br.offset == br.size);

    binary_free(&bw);
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
    char out[32] = { 0 };
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

/* =======================================================================
 * netaddr 字段访问 + 远端/本地地址获取
 * netaddr_remote / netaddr_local 通过 sock_pair 双 fd 互查验证；
 * netaddr_addr / netaddr_size / netaddr_ip / netaddr_port / netaddr_family
 * 在 IPv4 / IPv6 两种 family 下分别验证。
 * ======================================================================= */
static void test_netaddr_extra(CuTest *tc) {
    netaddr_ctx addr;
    char ipbuf[IP_LENS];

    // IPv4：字段访问
    CuAssertIntEquals(tc, ERR_OK, netaddr_set(&addr, "127.0.0.1", 12345));
    CuAssertIntEquals(tc, AF_INET, netaddr_family(&addr));
    CuAssertTrue(tc, 12345 == netaddr_port(&addr));
    CuAssertTrue(tc, (socklen_t)sizeof(struct sockaddr_in) == netaddr_size(&addr));
    CuAssertPtrNotNull(tc, netaddr_addr(&addr));
    CuAssertIntEquals(tc, ERR_OK, netaddr_ip(&addr, ipbuf));
    CuAssertStrEquals(tc, "127.0.0.1", ipbuf);

    // IPv6：字段访问
    CuAssertIntEquals(tc, ERR_OK, netaddr_set(&addr, "::1", 23456));
    CuAssertIntEquals(tc, AF_INET6, netaddr_family(&addr));
    CuAssertTrue(tc, 23456 == netaddr_port(&addr));
    CuAssertTrue(tc, (socklen_t)sizeof(struct sockaddr_in6) == netaddr_size(&addr));
    CuAssertIntEquals(tc, ERR_OK, netaddr_ip(&addr, ipbuf));
    CuAssertStrEquals(tc, "::1", ipbuf);

    // sock_pair 实测：netaddr_local / netaddr_remote
    // sock_pair 内部用 AF_INET TCP loopback 对，两端互为对端
    SOCKET fds[2];
    CuAssertIntEquals(tc, ERR_OK, sock_pair(fds));

    netaddr_ctx local0, remote0, local1, remote1;
    CuAssertIntEquals(tc, ERR_OK, netaddr_local(&local0, fds[0]));
    CuAssertIntEquals(tc, ERR_OK, netaddr_remote(&remote0, fds[0]));
    CuAssertIntEquals(tc, ERR_OK, netaddr_local(&local1, fds[1]));
    CuAssertIntEquals(tc, ERR_OK, netaddr_remote(&remote1, fds[1]));

    CuAssertIntEquals(tc, AF_INET, netaddr_family(&local0));
    CuAssertIntEquals(tc, AF_INET, netaddr_family(&remote0));

    // fds[0] 的 local 端口 = fds[1] 的 remote 端口
    CuAssertTrue(tc, netaddr_port(&local0) == netaddr_port(&remote1));
    CuAssertTrue(tc, netaddr_port(&local1) == netaddr_port(&remote0));

    // 两端 IP 都应为 127.0.0.1
    CuAssertIntEquals(tc, ERR_OK, netaddr_ip(&local0, ipbuf));
    CuAssertStrEquals(tc, "127.0.0.1", ipbuf);
    CuAssertIntEquals(tc, ERR_OK, netaddr_ip(&remote0, ipbuf));
    CuAssertStrEquals(tc, "127.0.0.1", ipbuf);

    CLOSE_SOCK(fds[0]);
    CLOSE_SOCK(fds[1]);
}

/* =======================================================================
 * binary —— binary_at / binary_offset / binary_set_va 补充
 * ======================================================================= */
static void test_binary_extra(CuTest *tc) {
    binary_ctx bw;

    /* ── binary_set_va：格式化写入 ── */
    binary_init(&bw, NULL, 0, 32);
    binary_set_va(&bw, "val=%d", 42);
    /* binary_set_va 写入 "val=42\0"，offset 停在 '\0' 前 */
    CuAssertTrue(tc, 6 == (int)bw.offset);
    CuAssertTrue(tc, 0 == memcmp("val=42", bw.data, 6));
    binary_free(&bw);

    /* ── binary_at：按位置取指针 ── */
    binary_init(&bw, NULL, 0, 32);
    binary_set_int8(&bw, 'A');
    binary_set_int8(&bw, 'B');
    binary_set_int8(&bw, 'C');
    CuAssertTrue(tc, 'A' == *binary_at(&bw, 0));
    CuAssertTrue(tc, 'B' == *binary_at(&bw, 1));
    CuAssertTrue(tc, 'C' == *binary_at(&bw, 2));
    binary_free(&bw);

    /* ── binary_offset (回填模式)：先占位，写内容后回到占位处回填 ── */
    binary_init(&bw, NULL, 0, 64);
    binary_set_skip(&bw, 4);                          /* 预留 4 字节长度字段 */
    size_t body_start = bw.offset;
    binary_set_binary(&bw, "body", 4);                /* 写入消息体（4 字节，无 \0）*/
    size_t body_end   = bw.offset;
    size_t body_len   = body_end - body_start;        /* 4 */

    binary_offset(&bw, 0);                            /* 绝对定位到起始 */
    binary_set_integer(&bw, (int64_t)body_len, 4, 0); /* 回填大端序长度 */
    binary_offset(&bw, body_end);                     /* 恢复到末尾 */

    /* 验证：从头读取长度字段和消息体 */
    binary_ctx br;
    binary_init(&br, bw.data, body_end, 0);
    uint32_t filled = (uint32_t)binary_get_uinteger(&br, 4, 0);
    CuAssertTrue(tc, 4 == (int)filled);
    const char *body = binary_get_binary(&br, 4);
    CuAssertTrue(tc, 0 == memcmp("body", body, 4));

    binary_free(&bw);
}

/* =======================================================================
 * buffer —— search 带起始偏移、drain 超量时的边界行为
 * ======================================================================= */
static void test_buffer_extra(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);

    /* 写入含两个分隔符的数据 */
    buffer_append(&buf, "aaa|bbb|ccc", 11);

    /* search 从 offset=0 查找第一个 '|'，返回绝对位置 3 */
    int pos = buffer_search(&buf, 0, 0, 0, "|", 1);
    CuAssertTrue(tc, 3 == pos);

    /* search 从 offset=4 查找第二个 '|'，返回绝对位置 7 */
    pos = buffer_search(&buf, 0, 4, 0, "|", 1);
    CuAssertTrue(tc, 7 == pos);

    /* search 查找不存在的子串，返回 ERR_FAILED */
    pos = buffer_search(&buf, 0, 0, 0, "xyz", 3);
    CuAssertTrue(tc, ERR_FAILED == pos);

    /* drain 请求量超出 buffer 大小时，仅删除实际数据 */
    size_t drained = buffer_drain(&buf, 1000);
    CuAssertTrue(tc, 11 == (int)drained);
    CuAssertTrue(tc, 0 == buffer_size(&buf));

    /* copyout 请求量超出时，返回实际可读字节数 */
    buffer_append(&buf, "hello", 5);
    char out[16];
    size_t nr = buffer_copyout(&buf, 0, out, 100);
    CuAssertTrue(tc, 5 == (int)nr);
    CuAssertTrue(tc, 0 == memcmp("hello", out, 5));

    /* 大小写不敏感搜索（ncs=1）*/
    buffer_drain(&buf, buffer_size(&buf));
    buffer_append(&buf, "Hello World", 11);
    pos = buffer_search(&buf, 1, 0, 0, "world", 5);
    CuAssertTrue(tc, 6 == pos);

    buffer_free(&buf);
}

/* =======================================================================
 * buffer_external（零拷贝注入外部数据）+ buffer_appendv（格式化追加）
 * buffer_external 接管外部 data，free_cb 会在 buffer_free 时被调用释放
 * buffer_appendv 按 printf 格式追加字符串数据
 * ======================================================================= */
static atomic_t _ext_free_called;
static void _ext_free(void *p) {
    ATOMIC_ADD(&_ext_free_called, 1);
    FREE(p);
}
static void test_buffer_external_appendv(CuTest *tc) {
    buffer_ctx buf;
    char readback[64];

    // buffer_external：零拷贝注入外部 MALLOC 的数据，free_cb 必须在 buffer_free 时调用
    ATOMIC_SET(&_ext_free_called, 0);
    buffer_init(&buf);

    char *ext;
    const char *src = "external-zero-copy";
    size_t slen = strlen(src);
    MALLOC(ext, slen);
    memcpy(ext, src, slen);

    buffer_external(&buf, ext, slen, _ext_free);
    CuAssertTrue(tc, slen == buffer_size(&buf));
    CuAssertTrue(tc, slen == buffer_copyout(&buf, 0, readback, slen));
    CuAssertTrue(tc, 0 == memcmp(src, readback, slen));

    buffer_free(&buf);
    // _ext_free 被调用一次
    CuAssertIntEquals(tc, 1, ATOMIC_GET(&_ext_free_called));

    // buffer_appendv：格式化追加
    buffer_init(&buf);
    CuAssertIntEquals(tc, ERR_OK, buffer_appendv(&buf, "n=%d s=%s", 42, "hi"));
    CuAssertTrue(tc, 9 == (int)buffer_size(&buf));
    CuAssertTrue(tc, 9 == buffer_copyout(&buf, 0, readback, 9));
    readback[9] = '\0';
    CuAssertStrEquals(tc, "n=42 s=hi", readback);

    // 连续 appendv 累加
    CuAssertIntEquals(tc, ERR_OK, buffer_appendv(&buf, " x=%x", 0xab));
    CuAssertTrue(tc, 14 == (int)buffer_size(&buf));
    CuAssertTrue(tc, 14 == buffer_copyout(&buf, 0, readback, 14));
    readback[14] = '\0';
    CuAssertStrEquals(tc, "n=42 s=hi x=ab", readback);

    buffer_free(&buf);
}

/* =======================================================================
 * buffer hint_node 悬空回归
 *   copyout(start>0) 把搜索游标缓存到某数据节点(hint_node)；随后 buffer_appendv
 *   触发 _buffer_expand_single 数据迁移、释放该节点。若不失效游标，下次 copyout
 *   会解引用已释放节点(ASan 下 heap-use-after-free)。本用例确定性命中迁移分支。
 * ======================================================================= */
static void test_buffer_hint_after_migrate(CuTest *tc) {
    buffer_ctx buf;
    buffer_init(&buf);

    // 1) 写 600 字节 → 单节点(buffer_lens≈968, off=600, free≈368, misalign=0)
    char seed[600];
    for (size_t i = 0; i < sizeof(seed); i++) {
        seed[i] = (char)('A' + (i % 26));
    }
    CuAssertIntEquals(tc, ERR_OK, buffer_append(&buf, seed, sizeof(seed)));
    CuAssertTrue(tc, sizeof(seed) == buffer_size(&buf));

    // 2) start>0 的 copyout 走 _buffer_search_start_cached → hint_node 指向该节点
    char out1[50];
    CuAssertTrue(tc, sizeof(out1) == buffer_copyout(&buf, 100, out1, sizeof(out1)));
    CuAssertTrue(tc, 0 == memcmp(out1, seed + 100, sizeof(out1)));

    // 3) appendv 一个 800 字节串(> free≈368) → _buffer_expand_single 迁移并释放原节点
    //    (原节点正是 hint_node；修复前此后 hint 悬空)
    char big[801];
    for (size_t i = 0; i + 1 < sizeof(big); i++) {
        big[i] = (char)('a' + (i % 26));
    }
    big[sizeof(big) - 1] = '\0';
    CuAssertIntEquals(tc, ERR_OK, buffer_appendv(&buf, "%s", big));
    CuAssertTrue(tc, (sizeof(seed) + sizeof(big) - 1) == buffer_size(&buf));

    // 4) 再次 start>0 copyout：命中已失效/重建的 hint。修复后读到原始字节；
    //    修复前 hint 指向已释放节点 → _buffer_search_start_cached 解引用即 UAF
    char out2[50];
    CuAssertTrue(tc, sizeof(out2) == buffer_copyout(&buf, 100, out2, sizeof(out2)));
    CuAssertTrue(tc, 0 == memcmp(out2, seed + 100, sizeof(out2)));

    // 5) 跨迁移边界读,确认整体数据完整(原 600 + 追加 800)
    char span[60];
    CuAssertTrue(tc, sizeof(span) == buffer_copyout(&buf, 580, span, sizeof(span)));
    CuAssertTrue(tc, 0 == memcmp(span, seed + 580, 20));     // [580,600) 原始尾
    CuAssertTrue(tc, 0 == memcmp(span + 20, big, 40));       // [600,640) 追加头
    buffer_free(&buf);
}

/* =======================================================================
 * varint —— MQTT 7-bit 变长编解码 + off>=blens 边界(回归)
 * ======================================================================= */
static void test_varint(CuTest *tc) {
    char enc[4];
    // 编码：字节数与上界溢出
    CuAssertIntEquals(tc, 1, varint_encode_mqtt(0, enc));
    CuAssertIntEquals(tc, 1, varint_encode_mqtt(127, enc));
    CuAssertIntEquals(tc, 2, varint_encode_mqtt(128, enc));
    CuAssertIntEquals(tc, 4, varint_encode_mqtt(0x0FFFFFFF, enc));
    CuAssertIntEquals(tc, 0, varint_encode_mqtt(0x10000000, enc));  // 超 256MB-1 上界

    // 编解码往返：300 → 2 字节
    buffer_ctx b;
    buffer_init(&b);
    int32_t n = varint_encode_mqtt(300, enc);
    CuAssertIntEquals(tc, 2, n);
    buffer_append(&b, enc, (size_t)n);
    size_t val = 0;
    CuAssertIntEquals(tc, 2, varint_decode_mqtt(&b, 0, buffer_size(&b), &val));
    CuAssertTrue(tc, 300 == val);
    buffer_free(&b);

    // 4 字节全延续位(0x80)；不能用字符串字面量："\x80\x80" 会被当成单个十六进制转义
    char allcont[4] = { (char)0x80, (char)0x80, (char)0x80, (char)0x80 };
    buffer_init(&b);
    buffer_append(&b, allcont, sizeof(allcont));

    // 4 字节内未结束 → ERR_FAILED
    val = 1;
    CuAssertIntEquals(tc, ERR_FAILED, varint_decode_mqtt(&b, 0, buffer_size(&b), &val));
    // off == blens：可读字节为 0 → ERR_FAILED
    CuAssertIntEquals(tc, ERR_FAILED, varint_decode_mqtt(&b, 4, buffer_size(&b), &val));
    // off > blens(回归点)：blens-off 无符号回绕,修复前越界读 buffer_at,修复后直接 ERR_FAILED
    val = 12345;
    CuAssertIntEquals(tc, ERR_FAILED, varint_decode_mqtt(&b, 9, buffer_size(&b), &val));
    CuAssertTrue(tc, 0 == val);  // 失败路径仍清零 *value
    buffer_free(&b);
}

/* =======================================================================
 * chan —— 缓冲收发、close 语义、并发生产者-消费者
 * ======================================================================= */

typedef struct { chan_ctx *ch; int items; } _chan_arg;

static void _chan_sender(void *arg) {
    _chan_arg *a = (_chan_arg *)arg;
    for (int i = 1; i <= a->items; i++) {
        uintptr_t v = (uintptr_t)i;
        chan_send(a->ch, (void *)v, 0, 0);
    }
}

static void test_chan(CuTest *tc) {
    /* ── 缓冲 chan（capacity=4）基本收发 ── */
    chan_ctx *ch = chan_init(4);
    CuAssertPtrNotNull(tc, ch);
    CuAssertTrue(tc, 0 == chan_is_closed(ch));
    CuAssertTrue(tc, 1 == chan_can_send(ch));

    /* 发送 3 个整数值（以指针携带，不拷贝，lens=0）*/
    for (uintptr_t i = 1; i <= 3; i++) {
        CuAssertTrue(tc, ERR_OK == chan_send(ch, (void *)i, 0, 0));
    }
    CuAssertTrue(tc, 3 == (int)chan_size(ch));

    /* 按序接收 */
    for (uintptr_t i = 1; i <= 3; i++) {
        size_t lens = 0;
        void *p = chan_recv(ch, &lens);
        CuAssertTrue(tc, (void *)i == p);
        CuAssertTrue(tc, 0 == (int)lens);
    }
    CuAssertTrue(tc, 0 == chan_size(ch));

    /* close 后发送应失败 */
    chan_close(ch);
    CuAssertTrue(tc, 1 == chan_is_closed(ch));
    CuAssertTrue(tc, ERR_OK != chan_send(ch, (void *)99, 0, 0));

    /* close 后接收返回 NULL（队列已空且已关闭）*/
    size_t lens = 0;
    CuAssertTrue(tc, NULL == chan_recv(ch, &lens));

    chan_free(ch);

    /* ── 并发：生产者线程发 1000 项，主线程收，验证总和 ── */
    ch = chan_init(64);
    _chan_arg arg = { ch, 1000 };
    pthread_t tid = thread_creat(_chan_sender, &arg);

    int64_t sum = 0;
    for (int i = 0; i < 1000; i++) {
        void *p = chan_recv(ch, &lens);
        sum += (int64_t)(uintptr_t)p;
    }
    thread_join(tid);
    /* 1+2+...+1000 = 500500 */
    CuAssertTrue(tc, 500500LL == sum);

    chan_free(ch);
}

/* =======================================================================
 * timer —— 初始化、当前时刻、计时
 * ======================================================================= */
static void test_timer(CuTest *tc) {
    timer_ctx t;
    timer_init(&t);

    /* timer_cur_ms 返回正值（启动后的当前时刻，ms 级）*/
    uint64_t ms1 = timer_cur_ms(&t);
    CuAssertTrue(tc, ms1 > 0);

    /* 连续两次调用，第二次 >= 第一次 */
    uint64_t ms2 = timer_cur_ms(&t);
    CuAssertTrue(tc, ms2 >= ms1);

    /* timer_start 后立即读取 elapsed，应 < 1000 ms（代码正常执行不会超过 1 秒）*/
    timer_start(&t);
    uint64_t e_ms = timer_elapsed_ms(&t);
    CuAssertTrue(tc, e_ms < 1000);

    /* timer_elapsed 纳秒值与 elapsed_ms 毫秒值量级一致 */
    timer_start(&t);
    uint64_t e_ns = timer_elapsed(&t);
    e_ms = timer_elapsed_ms(&t);
    /* 纳秒值不小于毫秒值 × 1000（允许少量误差，取一半） */
    CuAssertTrue(tc, e_ns >= e_ms * 500000ULL);
}

// load_trend：首次采样、上升不忙、下跌超阈值判定忙、紧贴阈值边界
static void test_load_trend(CuTest *tc) {
    load_trend_ctx trend;
    load_trend_init(&trend);
    // 首次采样：prev=0，无论 cur 多少都不忙
    CuAssertIntEquals(tc, 0, load_trend_busy(&trend, 100, 4, 5));
    CuAssertIntEquals(tc, 100, (int)trend.prev);

    // 持平：cur=prev → 不忙
    CuAssertIntEquals(tc, 0, load_trend_busy(&trend, 100, 4, 5));

    // 上升：cur > prev → 不忙
    CuAssertIntEquals(tc, 0, load_trend_busy(&trend, 200, 4, 5));
    CuAssertIntEquals(tc, 200, (int)trend.prev);

    // 跌幅 20% 阈值（4/5）：
    //   cur * 5 < prev * 4，即 cur < prev * 0.8
    //   prev=200，cur=159 → 159*5=795 < 200*4=800，触发繁忙
    CuAssertIntEquals(tc, 1, load_trend_busy(&trend, 159, 4, 5));
    CuAssertIntEquals(tc, 159, (int)trend.prev);

    // 跌幅刚好 20%：prev=200，cur=160 → 160*5=800 == 200*4=800，不忙（严格 < 才忙）
    load_trend_init(&trend);
    (void)load_trend_busy(&trend, 200, 4, 5);
    CuAssertIntEquals(tc, 0, load_trend_busy(&trend, 160, 4, 5));

    // 跌幅小于 20%（仅 10%）：prev=200，cur=180 → 不忙
    load_trend_init(&trend);
    (void)load_trend_busy(&trend, 200, 4, 5);
    CuAssertIntEquals(tc, 0, load_trend_busy(&trend, 180, 4, 5));

    // 不同阈值参数：跌幅超 50% (1/2) 才忙
    load_trend_init(&trend);
    (void)load_trend_busy(&trend, 1000, 1, 2);
    // cur=600 → 600*2=1200 vs 1000*1=1000，不忙（仍超过半）
    CuAssertIntEquals(tc, 0, load_trend_busy(&trend, 600, 1, 2));
    // 此时 prev=600，cur=200 → 200*2=400 < 600*1=600，跌幅 > 50% 判定忙
    CuAssertIntEquals(tc, 1, load_trend_busy(&trend, 200, 1, 2));

    // load_trend_init 重置后再次首采样不忙
    load_trend_init(&trend);
    CuAssertIntEquals(tc, 0, (int)trend.prev);
    CuAssertIntEquals(tc, 0, load_trend_busy(&trend, 5, 4, 5));
}

// timer 补充：timer_cur 纳秒 + 真实 sleep 后 elapsed 准确性 + 多次 elapsed 单调递增
static void test_timer_extra(CuTest *tc) {
    timer_ctx t;
    timer_init(&t);

    // timer_cur 纳秒值 > 0，且与 timer_cur_ms 量级一致
    uint64_t ns1 = timer_cur(&t);
    uint64_t ms1 = timer_cur_ms(&t);
    CuAssertTrue(tc, ns1 > 0);
    // ms1 在 ns1 / 1e6 附近（容差 ±100ms × 1e6 ns）
    uint64_t derived_ms = ns1 / 1000000ULL;
    int64_t diff = (int64_t)ms1 - (int64_t)derived_ms;
    if (diff < 0) diff = -diff;
    CuAssertTrue(tc, diff < 100);

    // 单调递增：连续两次 timer_cur，第二次 >= 第一次
    uint64_t ns2 = timer_cur(&t);
    CuAssertTrue(tc, ns2 >= ns1);

    // 真实 sleep 50ms 后 elapsed_ms 应接近 50ms（容差 ±30ms）
    timer_start(&t);
    MSLEEP(50);
    uint64_t e_ms = timer_elapsed_ms(&t);
    CuAssertTrue(tc, e_ms >= 30);
    CuAssertTrue(tc, e_ms < 200);

    // 多次 elapsed 单调递增
    timer_start(&t);
    uint64_t a = timer_elapsed(&t);
    uint64_t b = timer_elapsed(&t);
    uint64_t c = timer_elapsed(&t);
    CuAssertTrue(tc, b >= a);
    CuAssertTrue(tc, c >= b);

    // timer_start 重置起点：sleep + start + 立即 elapsed 应 < sleep 时长
    MSLEEP(20);
    timer_start(&t);
    uint64_t after_reset = timer_elapsed_ms(&t);
    // start 后立即读取，绝不应包含 sleep 的 20ms
    CuAssertTrue(tc, after_reset < 20);
}

/* =======================================================================
 * utils 杂项 —— createid / procscnt / nowms / contenttype / sectostr
 * ======================================================================= */
static void test_utils_misc(CuTest *tc) {
    /* createid：两次调用结果不同且非零 */
    uint64_t id1 = createid();
    uint64_t id2 = createid();
    CuAssertTrue(tc, 0 != id1);
    CuAssertTrue(tc, id1 != id2);

    /* parse_svid：手工构造边界值 */
    CuAssertIntEquals(tc, 0,      parse_svid((uint64_t)0));
    CuAssertIntEquals(tc, 0xFFFF, parse_svid((uint64_t)0xFFFF000000000000ULL));
    CuAssertIntEquals(tc, 0x7FFF, parse_svid((uint64_t)0x7FFF000000000000ULL));
    CuAssertIntEquals(tc, 0x1234, parse_svid((uint64_t)0x123456789ABCDEF0ULL));
    /* 低 48 位全 1 不应污染高 16 位 */
    CuAssertIntEquals(tc, 0,      parse_svid((uint64_t)0x0000FFFFFFFFFFFFULL));

    /* parse_svid + serviceid 往返：用 parse_svid 自身作为 getter 保存当前值,
     * 改 svid → 验证下次 createid 的高 16 位 → 还原,避免污染后续测试 */
    uint16_t saved = parse_svid(createid());
    CuAssertIntEquals(tc, ERR_OK, serviceid(0x42));
    CuAssertIntEquals(tc, 0x42, parse_svid(createid()));
    CuAssertIntEquals(tc, ERR_OK, serviceid(saved));
    CuAssertIntEquals(tc, saved, parse_svid(createid()));
    /* serviceid 拒绝 >= 0x8000 */
    CuAssertIntEquals(tc, ERR_FAILED, serviceid(0x8000));
    CuAssertIntEquals(tc, ERR_FAILED, serviceid(0xFFFF));
    CuAssertIntEquals(tc, saved, parse_svid(createid()));  /* svid 未被改写 */

    /* procscnt：至少 1 个逻辑核心 */
    CuAssertTrue(tc, procscnt() >= 1);

    /* nowms / nowsec：非零且量级一致（ms >= sec × 1000）*/
    uint64_t ms  = nowms();
    uint64_t sec = nowsec();
    CuAssertTrue(tc, ms  > 0);
    CuAssertTrue(tc, sec > 0);
    CuAssertTrue(tc, ms >= sec * 1000);

    /* contenttype：已知扩展名返回含对应关键字的字符串 */
    const char *ct = contenttype(".html");
    CuAssertPtrNotNull(tc, ct);
    CuAssertTrue(tc, strlen(ct) > 0);

    ct = contenttype(".html");
    CuAssertPtrNotNull(tc, ct);
    CuAssertTrue(tc, NULL != strstr(ct, "html"));

    /* 未知扩展名返回默认值（非 NULL）*/
    ct = contenttype(".unknownxyz");
    CuAssertPtrNotNull(tc, ct);

    /* sectostr / mstostr：转换结果非空 + 返回 ERR_OK */
    char timebuf[TIME_LENS];
    CuAssertIntEquals(tc, ERR_OK, sectostr(sec, "%Y-%m-%d %H:%M:%S", timebuf));
    CuAssertTrue(tc, strlen(timebuf) > 0);

    CuAssertIntEquals(tc, ERR_OK, mstostr(ms, "%Y-%m-%d %H:%M:%S", timebuf));
    CuAssertTrue(tc, strlen(timebuf) > 0);

    /* strtots：不断言绝对值(依赖进程时区/DST)，仅验证解析成功、畸形返 0、相邻秒差恒为 1(DST 偏移相减抵消) */
    uint64_t t0 = strtots("2026-06-15 12:00:00", "%Y-%m-%d %H:%M:%S");
    uint64_t t1 = strtots("2026-06-15 12:00:01", "%Y-%m-%d %H:%M:%S");
    CuAssertTrue(tc, t0 > 0 && t1 > 0);
    CuAssertTrue(tc, 1 == t1 - t0);
    CuAssertTrue(tc, 0 == strtots("not-a-date", "%Y-%m-%d %H:%M:%S"));
}

/* =======================================================================
 * popen2 —— 子进程启动、管道读写、等待退出
 * ======================================================================= */
static void test_popen2(CuTest *tc) {
    char script[PATH_LENS];
    char cmd[PATH_LENS + 16];
    popen_ctx ctx;
    char buf[256];
    int32_t n;
    const char *base = procpath();

#ifdef OS_WIN
    SNPRINTF(script, sizeof(script), "%s%s%s", base, PATH_SEPARATORSTR, "popen_echo.bat");
#else
    SNPRINTF(script, sizeof(script), "%s%s%s", base, PATH_SEPARATORSTR, "popen_echo.sh");
#endif

    /* 1. 无管道模式：仅启动并等待退出，验证退出码为 0 */
#ifdef OS_WIN
    SNPRINTF(cmd, sizeof(cmd), "cmd /c \"\"%s\"\"", script);
#else
    SNPRINTF(cmd, sizeof(cmd), "sh \"%s\"", script);
#endif
    CuAssertIntEquals(tc, ERR_OK, popen_startup(&ctx, cmd, NULL));
    CuAssertIntEquals(tc, ERR_OK, popen_waitexit(&ctx, 3000));
    CuAssertIntEquals(tc, 0, popen_exitcode(&ctx));
    popen_free(&ctx);

    /* 2. 只读模式：脚本输出固定字符串，验证读到 "hello popen" */
#ifdef OS_WIN
    SNPRINTF(cmd, sizeof(cmd), "cmd /c \"\"%s\" r\"", script);
#else
    SNPRINTF(cmd, sizeof(cmd), "sh \"%s\" r", script);
#endif
    CuAssertIntEquals(tc, ERR_OK, popen_startup(&ctx, cmd, "r"));
    CuAssertIntEquals(tc, ERR_OK, popen_waitexit(&ctx, 3000));
    ZERO(buf, sizeof(buf));
    n = popen_read(&ctx, buf, sizeof(buf) - 1);
    CuAssertTrue(tc, n > 0);
    CuAssertTrue(tc, NULL != strstr(buf, "hello popen"));
    popen_free(&ctx);

    /* 3. 读写模式：写入一行，等待脚本回显，验证读回内容一致 */
#ifdef OS_WIN
    SNPRINTF(cmd, sizeof(cmd), "cmd /c \"\"%s\" rw\"", script);
#else
    SNPRINTF(cmd, sizeof(cmd), "sh \"%s\" rw", script);
#endif
    CuAssertIntEquals(tc, ERR_OK, popen_startup(&ctx, cmd, "rw"));
    const char *msg = "srey test\n";
    n = popen_write(&ctx, msg, strlen(msg));
    CuAssertTrue(tc, n > 0);
    CuAssertIntEquals(tc, ERR_OK, popen_waitexit(&ctx, 3000));
    ZERO(buf, sizeof(buf));
    n = popen_read(&ctx, buf, sizeof(buf) - 1);
    CuAssertTrue(tc, n > 0);
    CuAssertTrue(tc, NULL != strstr(buf, "srey test"));
    popen_free(&ctx);
}

/* =======================================================================
 * log —— 日志等级 set/get
 * ======================================================================= */
static void test_log_lv(CuTest *tc) {
    /* 注：main.c 已 log_init(NULL, 0)，此处仅做 set/get 一致性验证，
     * 测试结束后还原默认级别避免影响后续日志输出 */
    LOG_LEVEL prev = log_getlv();

    log_setlv(LOGLV_FATAL);
    CuAssertIntEquals(tc, LOGLV_FATAL, log_getlv());
    log_setlv(LOGLV_ERROR);
    CuAssertIntEquals(tc, LOGLV_ERROR, log_getlv());
    log_setlv(LOGLV_WARN);
    CuAssertIntEquals(tc, LOGLV_WARN, log_getlv());
    log_setlv(LOGLV_INFO);
    CuAssertIntEquals(tc, LOGLV_INFO, log_getlv());
    log_setlv(LOGLV_DEBUG);
    CuAssertIntEquals(tc, LOGLV_DEBUG, log_getlv());

    /* 还原 */
    log_setlv(prev);
}

// slog 等级过滤路径：lv > _log_lv 时早返不入队，避免污染输出
// 注：mpq 入队/丢弃路径已由 test_mpq_concurrent_mc 覆盖，slog 入队路径无需重复测试
static void test_log_slog_filter(CuTest *tc) {
    (void)tc;
    LOG_LEVEL prev = log_getlv();
    // 设到 FATAL（最高级，值 0）：所有 lv > 0 的 slog 都被过滤
    log_setlv(LOGLV_FATAL);
    for (int i = 0; i < 100; i++) {
        slog(LOGLV_ERROR, "filtered error %d", i);
        slog(LOGLV_WARN,  "filtered warn %d",  i);
        slog(LOGLV_INFO,  "filtered info %d",  i);
        slog(LOGLV_DEBUG, "filtered debug %d", i);
    }
    log_setlv(prev);
}

/* =======================================================================
 * _strptime —— 字符串转时间
 * ======================================================================= */
static void test_strptime(CuTest *tc) {
    struct tm tm;

    /* %Y-%m-%d %H:%M:%S 完整匹配，返回指向结尾 '\0' 的位置 */
    ZERO(&tm, sizeof(tm));
    char *end = _strptime("2024-05-21 13:45:30", "%Y-%m-%d %H:%M:%S", &tm);
    CuAssertPtrNotNull(tc, end);
    CuAssertIntEquals(tc, 2024 - 1900, tm.tm_year);
    CuAssertIntEquals(tc, 5 - 1,      tm.tm_mon);
    CuAssertIntEquals(tc, 21,         tm.tm_mday);
    CuAssertIntEquals(tc, 13,         tm.tm_hour);
    CuAssertIntEquals(tc, 45,         tm.tm_min);
    CuAssertIntEquals(tc, 30,         tm.tm_sec);

    /* 单独日期 */
    ZERO(&tm, sizeof(tm));
    end = _strptime("1999-12-31", "%Y-%m-%d", &tm);
    CuAssertPtrNotNull(tc, end);
    CuAssertIntEquals(tc, 1999 - 1900, tm.tm_year);
    CuAssertIntEquals(tc, 12 - 1,     tm.tm_mon);
    CuAssertIntEquals(tc, 31,         tm.tm_mday);

    /* 单独时间 */
    ZERO(&tm, sizeof(tm));
    end = _strptime("09:08:07", "%H:%M:%S", &tm);
    CuAssertPtrNotNull(tc, end);
    CuAssertIntEquals(tc, 9, tm.tm_hour);
    CuAssertIntEquals(tc, 8, tm.tm_min);
    CuAssertIntEquals(tc, 7, tm.tm_sec);

    /* 不匹配的输入返回 NULL */
    ZERO(&tm, sizeof(tm));
    end = _strptime("not_a_date", "%Y-%m-%d", &tm);
    CuAssertTrue(tc, NULL == end);
}

/* =======================================================================
 * 时间轮 tw —— 启动 + 添加任务 + 等回调
 * ======================================================================= */
static atomic_t _tw_fired;

static void _tw_cb(ud_cxt *ud) {
    (void)ud;
    ATOMIC_ADD(&_tw_fired, 1);
}

static void test_tw(CuTest *tc) {
    tw_ctx tw;
    tw_init(&tw, 0, NULL); /* 0 → 默认容量 */

    _tw_fired = 0;
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));

    /* 投递 3 个 50ms 超时任务 */
    tw_add(&tw, 50, _tw_cb, NULL, &ud);
    tw_add(&tw, 50, _tw_cb, NULL, &ud);
    tw_add(&tw, 50, _tw_cb, NULL, &ud);

    /* 等待 500ms 让回调全部触发（含 tw 内部 mpq 入队 + jiffies 推进延迟）*/
    int32_t waited = 0;
    while (waited < 500 && ATOMIC_GET(&_tw_fired) < 3) {
        MSLEEP(20);
        waited += 20;
    }
    CuAssertIntEquals(tc, 3, ATOMIC_GET(&_tw_fired));

    tw_free(&tw);
}

/* =======================================================================
 * memichr / memstr / skipempty —— 字符/子串查找
 * ======================================================================= */
static void test_mem_helpers(CuTest *tc) {
    /* memichr：大小写不敏感字符查找 */
    const char *s = "Hello World";
    CuAssertTrue(tc, s + 0 == memichr(s, 'h', strlen(s)));
    CuAssertTrue(tc, s + 0 == memichr(s, 'H', strlen(s)));
    CuAssertTrue(tc, s + 6 == memichr(s, 'w', strlen(s)));
    CuAssertTrue(tc, NULL  == memichr(s, 'z', strlen(s)));

    /* memstr ncs=0：区分大小写 */
    CuAssertTrue(tc, s + 6 == memstr(0, s, strlen(s), "World", 5));
    CuAssertTrue(tc, NULL  == memstr(0, s, strlen(s), "world", 5));

    /* memstr ncs=1：大小写不敏感 */
    CuAssertTrue(tc, s + 6 == memstr(1, s, strlen(s), "world", 5));
    CuAssertTrue(tc, s + 0 == memstr(1, s, strlen(s), "HELLO", 5));
    CuAssertTrue(tc, NULL  == memstr(1, s, strlen(s), "zzz", 3));

    /* what 为空或更长于源 → NULL */
    CuAssertTrue(tc, NULL == memstr(0, s, 3, "Hello", 5));

    /* skipempty：跳过开头的空白字符（空格/\t/\r/\n）*/
    CuAssertTrue(tc, NULL != skipempty("   abc", 6));
    char *r = (char *)skipempty("   abc", 6);
    CuAssertTrue(tc, 'a' == *r);
    /* 全部空白 → NULL */
    CuAssertTrue(tc, NULL == skipempty("    ", 4));
    /* 无空白前缀 */
    r = (char *)skipempty("abc", 3);
    CuAssertTrue(tc, 'a' == *r);
}

/* =======================================================================
 * strupper / strlower / strreverse / tohex / split
 * ======================================================================= */
static void test_str_helpers(CuTest *tc) {
    /* strupper / strlower：原地修改 */
    char s1[16];
    safe_fill_str(s1, sizeof(s1), "Hello, World!");
    CuAssertTrue(tc, s1 == strupper(s1));
    CuAssertStrEquals(tc, "HELLO, WORLD!", s1);
    CuAssertTrue(tc, s1 == strlower(s1));
    CuAssertStrEquals(tc, "hello, world!", s1);

    /* strreverse：原地翻转 */
    char s2[16];
    safe_fill_str(s2, sizeof(s2), "abcdef");
    CuAssertTrue(tc, s2 == strreverse(s2));
    CuAssertStrEquals(tc, "fedcba", s2);

    /* 单字符翻转保持不变 */
    char s3[8];
    safe_fill_str(s3, sizeof(s3), "x");
    strreverse(s3);
    CuAssertStrEquals(tc, "x", s3);

    /* 空字符串安全 */
    char s4[2] = "";
    strreverse(s4);
    CuAssertStrEquals(tc, "", s4);

    /* tohex：二进制转 16 进制（大写）*/
    const uint8_t bin[] = { 0x00, 0xab, 0xff, 0x10 };
    char hex[HEX_ENSIZE(4)];
    /* HEX_ENSIZE = len*2+1，需手动补 '\0' */
    hex[HEX_ENSIZE(4) - 1] = '\0';
    CuAssertTrue(tc, hex == tohex(bin, 4, hex));
    CuAssertStrEquals(tc, "00ABFF10", hex);

    /* split：以 "," 拆分 */
    size_t n = 0;
    const char *str = "aa,bb,cc";
    buf_ctx *parts = split(str, strlen(str), ",", 1, &n);
    CuAssertPtrNotNull(tc, parts);
    CuAssertTrue(tc, 3 == n);
    CuAssertTrue(tc, 2 == parts[0].lens && 0 == memcmp(parts[0].data, "aa", 2));
    CuAssertTrue(tc, 2 == parts[1].lens && 0 == memcmp(parts[1].data, "bb", 2));
    CuAssertTrue(tc, 2 == parts[2].lens && 0 == memcmp(parts[2].data, "cc", 2));
    FREE(parts);

    /* split：分隔符在尾部 → 补空段 */
    const char *str2 = "x,y,";
    parts = split(str2, strlen(str2), ",", 1, &n);
    CuAssertPtrNotNull(tc, parts);
    CuAssertTrue(tc, 3 == n);
    CuAssertTrue(tc, 0 == parts[2].lens);
    FREE(parts);

    /* split：sep=NULL → 返回原始整段 */
    parts = split("hello", 5, NULL, 0, &n);
    CuAssertPtrNotNull(tc, parts);
    CuAssertTrue(tc, 1 == n);
    CuAssertTrue(tc, 5 == parts[0].lens);
    FREE(parts);
}

/* =======================================================================
 * format_va —— 短串走栈、长串走堆
 * ======================================================================= */
static void test_format_va(CuTest *tc) {
    /* 短串：栈缓冲分支 */
    char *s1 = format_va("val=%d name=%s", 42, "alice");
    CuAssertPtrNotNull(tc, s1);
    CuAssertStrEquals(tc, "val=42 name=alice", s1);
    FREE(s1);

    /* 空格式化 */
    char *s2 = format_va("%s", "");
    CuAssertPtrNotNull(tc, s2);
    CuAssertStrEquals(tc, "", s2);
    FREE(s2);

    /* 长串：触发堆分配分支（栈 buf=512，构造 1000 字符 'a'）*/
    char pad[1024];
    memset(pad, 'a', 1000);
    pad[1000] = '\0';
    char *s3 = format_va("%s", pad);
    CuAssertPtrNotNull(tc, s3);
    CuAssertTrue(tc, 1000 == (int)strlen(s3));
    CuAssertTrue(tc, 0 == memcmp(s3, pad, 1000));
    FREE(s3);
}

/* =======================================================================
 * hash / randrange / randstr / is_little / timeoffset / fill_timespec / threadid
 * ======================================================================= */
static void test_misc_helpers(CuTest *tc) {
    /* hash：相同输入相同输出，不同输入大概率不同 */
    uint64_t h1 = hash("hello", 5);
    uint64_t h2 = hash("hello", 5);
    uint64_t h3 = hash("world", 5);
    CuAssertTrue(tc, h1 == h2);
    CuAssertTrue(tc, h1 != h3);
    /* 空输入不崩溃 */
    hash("", 0);

    /* randrange [10, 20]：100 次均落在范围内 */
    for (int i = 0; i < 100; i++) {
        int32_t r = randrange(10, 20);
        CuAssertTrue(tc, r >= 10 && r <= 20);
    }
    /* min==max */
    CuAssertIntEquals(tc, 5, randrange(5, 5));

    /* randstr：写入 len 个字符 + 末尾 '\0' */
    char buf[33];
    CuAssertTrue(tc, buf == randstr(buf, 32));
    CuAssertIntEquals(tc, 32, (int)strlen(buf));
    /* 两次结果大概率不同 */
    char buf2[33];
    randstr(buf2, 32);
    CuAssertTrue(tc, 0 != memcmp(buf, buf2, 32));

    /* is_little：当前平台（macOS/Linux x86/ARM）均小端 */
    CuAssertIntEquals(tc, 1, is_little());

    /* timeoffset：分钟数，绝对值不超 24*60 */
    int32_t off = timeoffset();
    CuAssertTrue(tc, off > -24 * 60 && off < 24 * 60);

    /* fill_timespec：将相对毫秒数填入 timespec（非绝对时间）*/
    struct timespec ts;
    fill_timespec(&ts, 100);
    CuAssertTrue(tc, 0 == ts.tv_sec);
    CuAssertTrue(tc, 100 * 1000 * 1000 == ts.tv_nsec);
    fill_timespec(&ts, 1500);
    CuAssertTrue(tc, 1 == ts.tv_sec);
    CuAssertTrue(tc, 500 * 1000 * 1000 == ts.tv_nsec);
    fill_timespec(&ts, 0);
    CuAssertTrue(tc, 0 == ts.tv_sec && 0 == ts.tv_nsec);

    /* threadid：当前线程内多次调用一致 */
    uint64_t t1 = threadid();
    uint64_t t2 = threadid();
    CuAssertTrue(tc, t1 == t2);
    CuAssertTrue(tc, 0 != t1);
}

/* =======================================================================
 * ct_memcmp / secure_zero / csprng_rand
 * ======================================================================= */
static void test_security_helpers(CuTest *tc) {
    /* ct_memcmp：与 memcmp 同等价于"相等返回 0"，但内容差异不导致提前返回 */
    const char *a = "secretkey_aaa";
    const char *b = "secretkey_aaa";
    const char *c = "secretkey_bbb";
    CuAssertIntEquals(tc, 0, ct_memcmp(a, b, 13));
    CuAssertTrue(tc, 0 != ct_memcmp(a, c, 13));
    /* len=0 始终相等 */
    CuAssertIntEquals(tc, 0, ct_memcmp(a, c, 0));

    /* secure_zero：内容被清零 */
    char buf[16];
    memset(buf, 0xAB, sizeof(buf));
    secure_zero(buf, sizeof(buf));
    for (size_t i = 0; i < sizeof(buf); i++) {
        CuAssertTrue(tc, 0 == buf[i]);
    }
    /* NULL / len=0 安全 */
    secure_zero(NULL, 0);
    secure_zero(NULL, 16);
    secure_zero(buf, 0);

    /* csprng_rand：填充非全零（统计意义上 32 字节全零概率 2^-256，可忽略）*/
    char rnd[32];
    ZERO(rnd, sizeof(rnd));
    CuAssertIntEquals(tc, ERR_OK, csprng_rand(rnd, sizeof(rnd)));
    int allzero = 1;
    for (size_t i = 0; i < sizeof(rnd); i++) {
        if (0 != rnd[i]) {
            allzero = 0;
            break;
        }
    }
    CuAssertTrue(tc, !allzero);

    /* 两次调用结果大概率不同 */
    char rnd2[32];
    ZERO(rnd2, sizeof(rnd2));
    csprng_rand(rnd2, sizeof(rnd2));
    CuAssertTrue(tc, 0 != memcmp(rnd, rnd2, sizeof(rnd)));
}

/* =======================================================================
 * sock_pair —— 创建 TCP loopback 对，双向收发
 * ======================================================================= */
static void test_sock_pair(CuTest *tc) {
    SOCKET fds[2];
    CuAssertIntEquals(tc, ERR_OK, sock_pair(fds));
    CuAssertTrue(tc, INVALID_SOCK != fds[0]);
    CuAssertTrue(tc, INVALID_SOCK != fds[1]);

    /* fds[0] -> fds[1]：发送后多次轮询 recv */
    const char *msg = "hello sock_pair";
    int n = (int)send(fds[0], msg, (int)strlen(msg), 0);
    CuAssertTrue(tc, n == (int)strlen(msg));

    char rbuf[64];
    int got = 0;
    int waited = 0;
    while (waited < 1000) {
        int r = (int)recv(fds[1], rbuf + got, (int)(sizeof(rbuf) - 1 - got), 0);
        if (r > 0) {
            got += r;
            if (got >= (int)strlen(msg)) break;
        } else {
            MSLEEP(10);
            waited += 10;
        }
    }
    rbuf[got] = '\0';
    CuAssertTrue(tc, got == (int)strlen(msg));
    CuAssertTrue(tc, 0 == memcmp(rbuf, msg, strlen(msg)));

    /* 反方向 */
    const char *back = "ack";
    n = (int)send(fds[1], back, (int)strlen(back), 0);
    CuAssertTrue(tc, n == (int)strlen(back));

    got = 0;
    waited = 0;
    while (waited < 1000) {
        int r = (int)recv(fds[0], rbuf + got, (int)(sizeof(rbuf) - 1 - got), 0);
        if (r > 0) {
            got += r;
            if (got >= (int)strlen(back)) break;
        } else {
            MSLEEP(10);
            waited += 10;
        }
    }
    rbuf[got] = '\0';
    CuAssertTrue(tc, got == (int)strlen(back));
    CuAssertTrue(tc, 0 == memcmp(rbuf, back, strlen(back)));

    CLOSE_SOCK(fds[0]);
    CLOSE_SOCK(fds[1]);
}

/* =======================================================================
 * sock_* 配置 / 状态查询接口
 * 现有 test_sock_pair 仅验证了 sock_init/clean/pair 三个 API，
 * 本测试在 sock_pair 已建立的 TCP 对上集中验证：
 *   setter: sock_nodelay / sock_nonblock / sock_reuseaddr /
 *           sock_reuseport / sock_keepalive / sock_linger
 *   getter: sock_type / sock_family / sock_error / sock_nread / sock_checkconn
 * ======================================================================= */
static void test_sock_options(CuTest *tc) {
    SOCKET fds[2];
    CuAssertIntEquals(tc, ERR_OK, sock_pair(fds));

    // setter：sock_pair 内部已设过 nodelay/nonblock，但显式调用必须返回 ERR_OK
    CuAssertIntEquals(tc, ERR_OK, sock_nodelay(fds[0]));
    CuAssertIntEquals(tc, ERR_OK, sock_nonblock(fds[0]));
    // reuseaddr/reuseport/keepalive/linger 在已连接 fd 上仍可设置（影响新连接或关闭语义）
    CuAssertIntEquals(tc, ERR_OK, sock_reuseaddr(fds[0]));
    CuAssertIntEquals(tc, ERR_OK, sock_keepalive(fds[0], 60, 10));
    CuAssertIntEquals(tc, ERR_OK, sock_linger(fds[0]));
#if !defined(OS_WIN)
    // SO_REUSEPORT 在 Windows 不支持，跳过
    CuAssertIntEquals(tc, ERR_OK, sock_reuseport(fds[0]));
#endif

    // getter
    CuAssertIntEquals(tc, SOCK_STREAM, sock_type(fds[0]));
    CuAssertIntEquals(tc, AF_INET, sock_family(fds[0]));
    // 已 connect/accept 完成，sock_error 为 0（无 pending error），sock_checkconn ERR_OK
    CuAssertIntEquals(tc, 0, sock_error(fds[0]));
    CuAssertIntEquals(tc, ERR_OK, sock_checkconn(fds[0]));

    // sock_nread：发送数据后对端可读字节数应为发送长度
    const char *msg = "abcdef";
    int n = (int)send(fds[0], msg, (int)strlen(msg), 0);
    CuAssertTrue(tc, n == (int)strlen(msg));
    int waited = 0;
    int32_t nread = 0;
    while (waited < 1000) {
        nread = sock_nread(fds[1]);
        if (nread >= (int32_t)strlen(msg)) {
            break;
        }
        MSLEEP(10);
        waited += 10;
    }
    CuAssertTrue(tc, nread == (int32_t)strlen(msg));

    CLOSE_SOCK(fds[0]);
    CLOSE_SOCK(fds[1]);
}

/* =======================================================================
 * utils.h 文件系统 / 进程路径接口
 * isfile / isdir / filesize / readall / procpath
 * 用进程自身路径（procpath() 返回的目录，及其下已知存在的可执行文件）来构造测试输入
 * ======================================================================= */
static void test_utils_filesystem(CuTest *tc) {
    // procpath 应返回非空字符串（含末尾分隔符的目录路径）
    const char *dir = procpath();
    CuAssertPtrNotNull(tc, dir);
    CuAssertTrue(tc, strlen(dir) > 0);

    // procpath 指向的目录本身应被 isdir 识别为目录、不被 isfile 识别为文件
    CuAssertIntEquals(tc, ERR_OK, isdir(dir));
    CuAssertTrue(tc, ERR_OK != isfile(dir));

    // 临时文件：用 popen 创建，验证 isfile / filesize / readall
    char tmpfile[PATH_LENS];
    SNPRINTF(tmpfile, sizeof(tmpfile), "%s%stest_utils_fs.tmp", dir, PATH_SEPARATORSTR);

    const char *content = "hello-readall-1234567890";
    size_t clen = strlen(content);
    FILE *fp = fopen(tmpfile, "wb");
    CuAssertPtrNotNull(tc, fp);
    CuAssertTrue(tc, clen == fwrite(content, 1, clen, fp));
    fclose(fp);

    CuAssertIntEquals(tc, ERR_OK, isfile(tmpfile));
    CuAssertTrue(tc, ERR_OK != isdir(tmpfile));
    CuAssertTrue(tc, (int64_t)clen == filesize(tmpfile));

    size_t got = 0;
    char *data = readall(tmpfile, &got);
    CuAssertPtrNotNull(tc, data);
    CuAssertTrue(tc, clen == got);
    CuAssertTrue(tc, 0 == memcmp(content, data, clen));
    FREE(data);

    // 不存在的路径：isfile / isdir 返回非 ERR_OK，filesize 返回负值，readall 返回 NULL
    const char *bogus = "/nonexistent/path/to/nowhere_xyzzy";
    CuAssertTrue(tc, ERR_OK != isfile(bogus));
    CuAssertTrue(tc, ERR_OK != isdir(bogus));
    CuAssertTrue(tc, filesize(bogus) < 0);
    got = 0;
    CuAssertTrue(tc, NULL == readall(bogus, &got));

    // 清理
    remove(tmpfile);
}

/* =======================================================================
 * popen_close —— 子进程未结束时强制终止并回收
 * Unix: SIGKILL + waitpid 收尸，ctx->exited=1 / exitcode=ERR_FAILED
 * Windows: TerminateProcess
 * ======================================================================= */
static void test_popen_close(CuTest *tc) {
    popen_ctx ctx;
    char cmd[64];
#ifdef OS_WIN
    SNPRINTF(cmd, sizeof(cmd), "cmd /c \"timeout /t 30 /nobreak\"");
#else
    SNPRINTF(cmd, sizeof(cmd), "sh -c 'sleep 30'");
#endif
    /* 启动 30 秒长任务，无管道模式即可 */
    CuAssertIntEquals(tc, ERR_OK, popen_startup(&ctx, cmd, NULL));

    /* 立即 popen_close 应在毫秒级返回（SIGKILL + waitpid 同步收尸） */
    uint64_t t0 = nowms();
    popen_close(&ctx);
    uint64_t elapsed = nowms() - t0;
    CuAssertTrue(tc, elapsed < 5000); /* 5s 内必结束（实际应远低于 100ms） */
#ifndef OS_WIN
    /* Unix 下 popen_close 自带 waitpid，exited 标志置 1 */
    CuAssertIntEquals(tc, 1, ctx.exited);
    CuAssertIntEquals(tc, ERR_FAILED, ctx.exitcode);
#endif
    popen_free(&ctx);

    /* close 后再次 close 应为 no-op（idempotent，不应 crash 不应阻塞） */
    CuAssertIntEquals(tc, ERR_OK, popen_startup(&ctx, cmd, NULL));
    popen_close(&ctx);
    popen_close(&ctx);  /* 重复调用：pid 仍非 0 但 exited=1，分支 if 不进入 kill */
    popen_free(&ctx);
}

/* =======================================================================
 * sfid_init 非法参数返回 NULL（位数越界、机器ID越界、customepoch >= now）
 * ======================================================================= */
static void test_sfid_invalid(CuTest *tc) {
    sfid_ctx ctx;
    /* machinebitlen + sequencebitlen > 22 拒绝 */
    CuAssertTrue(tc, NULL == sfid_init(&ctx, 0, 12, 12, 0));
    /* machinebitlen 不限上界，但 + sequencebitlen 总和 > 22 即拒绝 */
    CuAssertTrue(tc, NULL == sfid_init(&ctx, 0, 21, 2, 0));
    /* machineid 超出 [0, 2^bitlen-1] 范围 */
    CuAssertTrue(tc, NULL == sfid_init(&ctx, 1024, 10, 12, 0)); /* 2^10=1024 */
    CuAssertTrue(tc, NULL == sfid_init(&ctx, -1, 10, 12, 0));
    /* customepoch >= 当前时间（取未来时间戳） */
    uint64_t future = nowms() + 60000;
    CuAssertTrue(tc, NULL == sfid_init(&ctx, 0, 0, 0, future));
    /* 合法边界：bitlen=1+1，machineid=1 → 1bit 上限是 1 */
    CuAssertPtrNotNull(tc, sfid_init(&ctx, 1, 1, 1, 0));
    /* bitlen 总和恰好 22 合法 */
    CuAssertPtrNotNull(tc, sfid_init(&ctx, 0, 10, 12, 0));
}

/* =======================================================================
 * hash_ring 边界场景：空环查找、NULL 入参、重复添加、replicas=0、移除不存在
 * ======================================================================= */
static void test_hash_ring_edge(CuTest *tc) {
    hash_ring_ctx ring;
    hash_ring_init(&ring);

    /* 空环查找返回 NULL */
    CuAssertTrue(tc, NULL == hash_ring_find(&ring, "any", 3));

    /* NULL 入参拒绝 */
    CuAssertIntEquals(tc, ERR_FAILED, hash_ring_add(&ring, NULL, 5, 10));
    CuAssertIntEquals(tc, ERR_FAILED, hash_ring_add(&ring, "n", 0, 10));
    CuAssertIntEquals(tc, ERR_FAILED, hash_ring_add(&ring, "n", 1, 0));
    CuAssertTrue(tc, NULL == hash_ring_find(&ring, NULL, 3));
    CuAssertTrue(tc, NULL == hash_ring_find(&ring, "k", 0));

    /* 添加 + 重复添加同名 → 拒绝 */
    CuAssertIntEquals(tc, ERR_OK,     hash_ring_add(&ring, "nodeA", 5, 100));
    CuAssertIntEquals(tc, ERR_FAILED, hash_ring_add(&ring, "nodeA", 5, 100));
    CuAssertTrue(tc, 1 == ring.nnodes);

    /* 移除不存在的节点：无副作用，不崩溃 */
    hash_ring_remove(&ring, "ghost", 5);
    CuAssertTrue(tc, 1 == ring.nnodes);

    /* 移除已存在的节点，环回空 */
    hash_ring_remove(&ring, "nodeA", 5);
    CuAssertTrue(tc, 0 == ring.nnodes);
    CuAssertTrue(tc, 0 == ring.nitems);
    CuAssertTrue(tc, NULL == hash_ring_find(&ring, "any", 3));

    /* 名称超过 NAME_STACK_LEN(512) 触发堆分配路径 */
    char long_name[600];
    memset(long_name, 'x', sizeof(long_name));
    CuAssertIntEquals(tc, ERR_OK, hash_ring_add(&ring, long_name, sizeof(long_name), 50));
    CuAssertTrue(tc, 1 == ring.nnodes);

    hash_ring_free(&ring);
}

/* =======================================================================
 * _strptime 非法输入：字段越界 / 字面量不匹配 / 未知转换符
 * ======================================================================= */
static void test_strptime_invalid(CuTest *tc) {
    struct tm tm;

    /* 月份越界：13 超出 [1,12] */
    ZERO(&tm, sizeof(tm));
    CuAssertTrue(tc, NULL == _strptime("2024-13-01", "%Y-%m-%d", &tm));

    /* 小时越界：24 超出 [0,23] */
    ZERO(&tm, sizeof(tm));
    CuAssertTrue(tc, NULL == _strptime("24:00:00", "%H:%M:%S", &tm));

    /* 分钟越界：60 超出 [0,59] */
    ZERO(&tm, sizeof(tm));
    CuAssertTrue(tc, NULL == _strptime("12:60:00", "%H:%M:%S", &tm));

    /* 字面量不匹配：'/' vs '-' */
    ZERO(&tm, sizeof(tm));
    CuAssertTrue(tc, NULL == _strptime("2024/05/21", "%Y-%m-%d", &tm));

    /* 完全非数字输入 */
    ZERO(&tm, sizeof(tm));
    CuAssertTrue(tc, NULL == _strptime("notnumber", "%Y", &tm));

    /* 未知转换符 %X 不支持 → NULL（fallthrough 由实现处理） */
    ZERO(&tm, sizeof(tm));
    /* %Q 不存在 */
    CuAssertTrue(tc, NULL == _strptime("2024", "%Q", &tm));

    /* 日期越界：32 日 */
    ZERO(&tm, sizeof(tm));
    CuAssertTrue(tc, NULL == _strptime("2024-05-32", "%Y-%m-%d", &tm));
}

/* =======================================================================
 * _strptime 周序(%U/%W)高周数：tm_yday>=366 时月份归算不越界读
 * 回归 start_of_month[2][13] 行尾越界：旧循环无上界，tm_yday>=366 时会读到
 * start_of_month[isleap][13]（越界一格）才被随后的 i>12 跨年归一兜住；
 * ASan 构建(sh mk.sh test asan debug)下可稳定捕获该越界读
 * ======================================================================= */
static void test_strptime_week_rollover(CuTest *tc) {
    struct tm tm;
    char *end;

    /* %U（周日为周首）第 53 周 + 周六 → tm_yday≈376>=366，触发月份归算上界 + 跨年归一到 2025 */
    ZERO(&tm, sizeof(tm));
    end = _strptime("2024 53 6", "%Y %U %w", &tm);
    CuAssertPtrNotNull(tc, end);
    CuAssertTrue(tc, '\0' == *end);
    CuAssertTrue(tc, 125 == tm.tm_year);
    CuAssertTrue(tc, 0 == tm.tm_mon);
    CuAssertTrue(tc, tm.tm_mday >= 1 && tm.tm_mday <= 31);

    /* %W（周一为周首）第 53 周 → 同样 tm_yday>=366 触发越界路径 */
    ZERO(&tm, sizeof(tm));
    end = _strptime("2024 53 6", "%Y %W %w", &tm);
    CuAssertPtrNotNull(tc, end);
    CuAssertTrue(tc, '\0' == *end);
    CuAssertTrue(tc, 125 == tm.tm_year);
    CuAssertTrue(tc, 0 == tm.tm_mon);
    CuAssertTrue(tc, tm.tm_mday >= 1 && tm.tm_mday <= 31);

    /* 常规低周数：不溢出，月份正常归算（确保上界修复未误伤常规路径） */
    ZERO(&tm, sizeof(tm));
    end = _strptime("2024 10 3", "%Y %U %w", &tm);
    CuAssertPtrNotNull(tc, end);
    CuAssertTrue(tc, '\0' == *end);
    CuAssertTrue(tc, 124 == tm.tm_year);
    CuAssertTrue(tc, tm.tm_mon >= 0 && tm.tm_mon <= 11);
    CuAssertTrue(tc, tm.tm_mday >= 1 && tm.tm_mday <= 31);
}

/* =======================================================================
 * %U/%W 第 0 周 + 小 wday → tm_yday 为负数 → 归一到上一年
 * 覆盖 strptime.c 负 tm_yday 回滚路径（与 test_strptime_week_rollover 的正溢互补）
 * ======================================================================= */
static void test_strptime_week_neg_yday(CuTest *tc) {
    struct tm tm;
    char *end;

    // 2023-01-01 是周日(fwd=0)，%U 第 0 周 + wday=0 → tm_yday = -7 → 2022-12-25
    ZERO(&tm, sizeof(tm));
    end = _strptime("2023 0 0", "%Y %U %w", &tm);
    CuAssertPtrNotNull(tc, end);
    CuAssertTrue(tc, '\0' == *end);
    CuAssertTrue(tc, 122 == tm.tm_year);
    CuAssertTrue(tc, 11 == tm.tm_mon);
    CuAssertTrue(tc, 25 == tm.tm_mday);

    // 2024-01-01 是周一(fwd=1)，%W 第 0 周 + wday=0 → tm_yday = -8 → 2023-12-24
    ZERO(&tm, sizeof(tm));
    end = _strptime("2024 0 0", "%Y %W %w", &tm);
    CuAssertPtrNotNull(tc, end);
    CuAssertTrue(tc, '\0' == *end);
    CuAssertTrue(tc, 123 == tm.tm_year);
    CuAssertTrue(tc, 11 == tm.tm_mon);
    CuAssertTrue(tc, 24 == tm.tm_mday);

    // 常规路径（week>0）不受影响：%U 第 1 周 + 周三 = 2024-01-10
    ZERO(&tm, sizeof(tm));
    end = _strptime("2024 1 3", "%Y %U %w", &tm);
    CuAssertPtrNotNull(tc, end);
    CuAssertTrue(tc, '\0' == *end);
    CuAssertTrue(tc, 124 == tm.tm_year);
    CuAssertTrue(tc, 0 == tm.tm_mon);
    CuAssertTrue(tc, 10 == tm.tm_mday);
}

/* =======================================================================
 * tw 长超时路径：> 256ms 进入 tv2 槽
 * （tv1 容量 256，超过即下沉到 tv2，验证 cascade 后回调仍正确触发）
 * ======================================================================= */
static void test_tw_long_timeout(CuTest *tc) {
    tw_ctx tw;
    tw_init(&tw, 0, NULL);

    _tw_fired = 0;
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));

    /* 500ms 超时进入 tv2 槽，cascade 后回调正常触发 */
    tw_add(&tw, 500, _tw_cb, NULL, &ud);

    /* 1.2s 内必触发，留余量给 cascade + 调度 */
    int32_t waited = 0;
    while (waited < 1200 && ATOMIC_GET(&_tw_fired) < 1) {
        MSLEEP(50);
        waited += 50;
    }
    CuAssertIntEquals(tc, 1, ATOMIC_GET(&_tw_fired));

    tw_free(&tw);
}

/* =======================================================================
 * pool —— 对象池:取/还/复用、满处理、收缩、释放(thsafe=0 queue / thsafe=1 fsqu)
 * ======================================================================= */
// 带标记的测试对象:_elfree 收到真实对象时 magic 必为 POOL_T_MAGIC;
// 若收到队列槽位地址(历史 bug),magic 不符,_pt_free_bad 增长
#define POOL_T_MAGIC 0x5ada5adau
typedef struct pool_t_obj {
    uint32_t magic;      // _elnew 置 POOL_T_MAGIC
    uint32_t reset_cnt;  // 本对象被 _elreset 次数
    uint32_t clear_cnt;  // 本对象被 _elclear 次数
} pool_t_obj;
// 回调无 user-data,用文件静态累计各回调次数与最近一次透传 args
static uint32_t _pt_new, _pt_free, _pt_reset, _pt_clear, _pt_free_bad;
static void *_pt_new_args, *_pt_reset_args;
static void _pt_counters_reset(void) {
    _pt_new = _pt_free = _pt_reset = _pt_clear = _pt_free_bad = 0;
    _pt_new_args = _pt_reset_args = NULL;
}
static void *_pt_elnew(void *args) {
    pool_t_obj *o;
    CALLOC(o, 1, sizeof(pool_t_obj));
    o->magic = POOL_T_MAGIC;
    _pt_new++;
    _pt_new_args = args;
    return o;
}
static void _pt_elfree(void *data) {
    pool_t_obj *o = (pool_t_obj *)data;
    if (POOL_T_MAGIC != o->magic) {
        _pt_free_bad++;
    }
    _pt_free++;
    FREE(o);
}
static void _pt_elreset(void *data, void *args) {
    ((pool_t_obj *)data)->reset_cnt++;
    _pt_reset++;
    _pt_reset_args = args;
}
static void _pt_elclear(void *data) {
    ((pool_t_obj *)data)->clear_cnt++;
    _pt_clear++;
}
static el_cbs _pt_cbs = { _pt_elnew, _pt_elfree, _pt_elreset, _pt_elclear };

// 取/还/复用:空池 pop 走 _elnew,push 走 _elclear,命中 pop 走 _elreset(不再 new),args 透传
static void _pool_basic_check(CuTest *tc, int32_t thsafe) {
    pool_ctx pool;
    pool_t_obj *o, *o2;
    int32_t arg = 7;
    _pt_counters_reset();
    pool_init(&pool, sizeof(pool_t_obj), 8, 2, thsafe, &_pt_cbs);
    CuAssertIntEquals(tc, 0, pool_size(&pool));
    CuAssertTrue(tc, pool_capacity(&pool) >= 8);
    o = (pool_t_obj *)pool_pop(&pool, &arg);
    CuAssertPtrNotNull(tc, o);
    CuAssertTrue(tc, POOL_T_MAGIC == o->magic);
    CuAssertIntEquals(tc, 1, _pt_new);
    CuAssertPtrEquals(tc, &arg, _pt_new_args);
    CuAssertIntEquals(tc, 0, pool_size(&pool));
    pool_push(&pool, o);
    CuAssertIntEquals(tc, 1, _pt_clear);
    CuAssertIntEquals(tc, 1, pool_size(&pool));
    o2 = (pool_t_obj *)pool_pop(&pool, &arg);
    CuAssertPtrEquals(tc, o, o2); // 命中复用,同一对象
    CuAssertIntEquals(tc, 1, _pt_new); // 未新建
    CuAssertIntEquals(tc, 1, _pt_reset);
    CuAssertPtrEquals(tc, &arg, _pt_reset_args);
    CuAssertIntEquals(tc, 0, pool_size(&pool));
    pool_push(&pool, o2);
    pool_free(&pool);
    CuAssertIntEquals(tc, 1, _pt_free);
    CuAssertIntEquals(tc, 0, _pt_free_bad);
}
static void test_pool_basic(CuTest *tc) {
    _pool_basic_check(tc, 0);
    _pool_basic_check(tc, 1);
}
// 满池:pool_push 满则 _elfree;pool_trypush 满则返回 ERR_FAILED 且不接管对象
static void test_pool_full(CuTest *tc) {
    pool_ctx pool;
    pool_t_obj *objs[4], *over;
    uint32_t i;
    int32_t rt;
    _pt_counters_reset();
    pool_init(&pool, sizeof(pool_t_obj), 4, 0, 0, &_pt_cbs);
    CuAssertIntEquals(tc, 4, pool_capacity(&pool));
    for (i = 0; i < 4; i++) {
        objs[i] = (pool_t_obj *)pool_pop(&pool, NULL);
    }
    for (i = 0; i < 4; i++) {
        pool_push(&pool, objs[i]);
    }
    CuAssertIntEquals(tc, 4, pool_size(&pool));
    CuAssertIntEquals(tc, 0, _pt_free);
    // 满池 pool_push:对象被释放,size 不变
    over = (pool_t_obj *)_pt_elnew(NULL);
    pool_push(&pool, over);
    CuAssertIntEquals(tc, 1, _pt_free);
    CuAssertIntEquals(tc, 4, pool_size(&pool));
    // 满池 pool_trypush:返回 ERR_FAILED 且不释放,对象仍归调用方
    over = (pool_t_obj *)_pt_elnew(NULL);
    rt = pool_trypush(&pool, over);
    CuAssertIntEquals(tc, ERR_FAILED, rt);
    CuAssertIntEquals(tc, 1, _pt_free);
    CuAssertIntEquals(tc, 4, pool_size(&pool));
    FREE(over); // trypush 满未接管,调用方自行释放
    pool_free(&pool);
    CuAssertIntEquals(tc, 5, _pt_free); // 1(push 满)+4(pool_free)
    CuAssertIntEquals(tc, 0, _pt_free_bad);
}
// 收缩:释放至 max(keep,nkeep),且交给 _elfree 的都是真实对象(magic 正确)——历史 bug 回归点
static void _pool_shrink_check(CuTest *tc, int32_t thsafe) {
    pool_ctx pool;
    pool_t_obj *objs[8];
    uint32_t i;
    _pt_counters_reset();
    pool_init(&pool, sizeof(pool_t_obj), 16, 2, thsafe, &_pt_cbs);
    for (i = 0; i < 8; i++) {
        objs[i] = (pool_t_obj *)pool_pop(&pool, NULL);
    }
    for (i = 0; i < 8; i++) {
        pool_push(&pool, objs[i]);
    }
    CuAssertIntEquals(tc, 8, pool_size(&pool));
    pool_shrink(&pool, 3, 4, 5); // keep=max(3,nkeep=2)=3,释放 5
    CuAssertIntEquals(tc, 5, _pt_free);
    CuAssertIntEquals(tc, 0, _pt_free_bad); // bug 版收到槽位地址,magic 不符则 >0
    CuAssertIntEquals(tc, 3, pool_size(&pool));
    pool_free(&pool);
    CuAssertIntEquals(tc, 8, _pt_free);
    CuAssertIntEquals(tc, 0, _pt_free_bad);
}
static void test_pool_shrink(CuTest *tc) {
    _pool_shrink_check(tc, 0); // 非线程安全 queue —— 本次修复路径
    _pool_shrink_check(tc, 1); // 线程安全 fsqu —— 确认仍正确
}
// 收缩策略:nkeep 下限 与 load_trend busy 跳过(thsafe=0,确定性)
static void test_pool_shrink_policy(CuTest *tc) {
    pool_ctx pool;
    pool_t_obj *objs[8];
    uint32_t i;
    // nkeep 下限:keep 小于 nkeep 时仍保留 nkeep 个
    _pt_counters_reset();
    pool_init(&pool, sizeof(pool_t_obj), 16, 2, 0, &_pt_cbs);
    for (i = 0; i < 8; i++) {
        objs[i] = (pool_t_obj *)pool_pop(&pool, NULL);
    }
    for (i = 0; i < 8; i++) {
        pool_push(&pool, objs[i]);
    }
    pool_shrink(&pool, 0, 4, 5); // 首次 prev=0 不忙;keep=max(0,2)=2
    CuAssertIntEquals(tc, 2, pool_size(&pool));
    pool_free(&pool);
    // busy 跳过:采样骤降(8→2,跌幅 >20%)后,下一次收缩被跳过
    _pt_counters_reset();
    pool_init(&pool, sizeof(pool_t_obj), 16, 0, 0, &_pt_cbs);
    for (i = 0; i < 8; i++) {
        objs[i] = (pool_t_obj *)pool_pop(&pool, NULL);
    }
    for (i = 0; i < 8; i++) {
        pool_push(&pool, objs[i]);
    }
    pool_shrink(&pool, 2, 4, 5); // 首次:不忙,8→2,记录 prev=8
    CuAssertIntEquals(tc, 2, pool_size(&pool));
    pool_shrink(&pool, 0, 4, 5); // cur=2 < prev(8)*4/5 → 忙 → 跳过
    CuAssertIntEquals(tc, 2, pool_size(&pool));
    pool_free(&pool);
}
// 默认回调(NULL):走 CALLOC/FREE;容量 0 用默认值
static void test_pool_default(CuTest *tc) {
    pool_ctx pool;
    uint64_t *a, *b;
    pool_init(&pool, sizeof(uint64_t), 0, 4, 0, NULL);
    CuAssertTrue(tc, pool_capacity(&pool) >= 1024); // POOL_DEFAULT_CAP
    a = (uint64_t *)pool_pop(&pool, NULL);
    CuAssertPtrNotNull(tc, a);
    CuAssertTrue(tc, 0 == *a); // CALLOC 清零
    *a = 12345;
    pool_push(&pool, a);
    b = (uint64_t *)pool_pop(&pool, NULL);
    CuAssertPtrEquals(tc, a, b); // 命中复用
    pool_push(&pool, b);
    pool_free(&pool);
}

// TDA-1：threshold 翻倍溢出修复验证
// overload = SIZE_MAX, init = 1 → 不死循环，threshold 钳至 SIZE_MAX，返回 1
static void test_tda_overflow(CuTest *tc) {
    tda_ctx ctx;
    // 普通路径：触发 1→2→4
    tda_init(&ctx, 1);
    CuAssertIntEquals(tc, 1, tda_check(&ctx, 2));
    CuAssertIntEquals(tc, 4, (int)ctx.overload_threshold);
    CuAssertIntEquals(tc, 0, tda_check(&ctx, 3));
    CuAssertIntEquals(tc, 1, tda_check(&ctx, 5));
    CuAssertIntEquals(tc, 8, (int)ctx.overload_threshold);
    // 复位
    CuAssertIntEquals(tc, 0, tda_check(&ctx, 0));
    CuAssertIntEquals(tc, 1, (int)ctx.overload_threshold);
    // 上溢路径：overload = SIZE_MAX → 不死循环，threshold 被钳为 SIZE_MAX
    tda_init(&ctx, 1);
    CuAssertIntEquals(tc, 1, tda_check(&ctx, SIZE_MAX));
    CuAssertTrue(tc, ctx.overload_threshold == SIZE_MAX);
}

/* ======================================================================= */

void test_utils(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_pack_unpack);
    SUITE_ADD_TEST(suite, test_binary);
    SUITE_ADD_TEST(suite, test_binary_extra);
    SUITE_ADD_TEST(suite, test_buffer);
    SUITE_ADD_TEST(suite, test_buffer_extra);
    SUITE_ADD_TEST(suite, test_buffer_external_appendv);
    SUITE_ADD_TEST(suite, test_buffer_hint_after_migrate);
    SUITE_ADD_TEST(suite, test_varint);
    SUITE_ADD_TEST(suite, test_sfid);
    SUITE_ADD_TEST(suite, test_sfid_invalid);
    SUITE_ADD_TEST(suite, test_hash_ring);
    SUITE_ADD_TEST(suite, test_hash_ring_edge);
    SUITE_ADD_TEST(suite, test_netaddr);
    SUITE_ADD_TEST(suite, test_netaddr_extra);
    SUITE_ADD_TEST(suite, test_chan);
    SUITE_ADD_TEST(suite, test_timer);
    SUITE_ADD_TEST(suite, test_timer_extra);
    SUITE_ADD_TEST(suite, test_load_trend);
    SUITE_ADD_TEST(suite, test_utils_misc);
    SUITE_ADD_TEST(suite, test_popen2);
    SUITE_ADD_TEST(suite, test_popen_close);
    SUITE_ADD_TEST(suite, test_log_lv);
    SUITE_ADD_TEST(suite, test_log_slog_filter);
    SUITE_ADD_TEST(suite, test_strptime);
    SUITE_ADD_TEST(suite, test_strptime_invalid);
    SUITE_ADD_TEST(suite, test_strptime_week_rollover);
    SUITE_ADD_TEST(suite, test_strptime_week_neg_yday);
    SUITE_ADD_TEST(suite, test_tw);
    SUITE_ADD_TEST(suite, test_tw_long_timeout);
    SUITE_ADD_TEST(suite, test_mem_helpers);
    SUITE_ADD_TEST(suite, test_str_helpers);
    SUITE_ADD_TEST(suite, test_format_va);
    SUITE_ADD_TEST(suite, test_misc_helpers);
    SUITE_ADD_TEST(suite, test_security_helpers);
    SUITE_ADD_TEST(suite, test_sock_pair);
    SUITE_ADD_TEST(suite, test_sock_options);
    SUITE_ADD_TEST(suite, test_utils_filesystem);
    SUITE_ADD_TEST(suite, test_pool_basic);
    SUITE_ADD_TEST(suite, test_pool_full);
    SUITE_ADD_TEST(suite, test_pool_shrink);
    SUITE_ADD_TEST(suite, test_pool_shrink_policy);
    SUITE_ADD_TEST(suite, test_pool_default);
    SUITE_ADD_TEST(suite, test_tda_overflow);
}
