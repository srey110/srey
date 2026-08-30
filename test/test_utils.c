#include "test_utils.h"
#include "lib.h"
#include "utils/strptime.h"
#include "utils/pool.h"
#include <locale.h>

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

/* buf 指向 ctx 自己的缓冲时，_binary_expand 的 REALLOC 会把源搬走，
 * 改前 memcpy 读的是已释放的旧块（ASan 下直接报 heap-use-after-free）。
 * 初始容量 256，先写 200 再自追加 200 必然触发扩容。
 * set_binary 与 set_string 共用 _binary_append，两条入口都要覆盖 */
static void test_binary_set_binary_self_alias(CuTest *tc) {
    binary_ctx bin;
    char pad[200];
    char snap[200];
    memset(pad, 'x', sizeof(pad));
    binary_init(&bin, NULL, 0, 0);
    binary_set_binary(&bin, pad, sizeof(pad));
    memcpy(snap, bin.data, bin.offset);
    size_t before = bin.offset;

    binary_set_binary(&bin, bin.data, before);
    CuAssertTrue(tc, 2 * before == bin.offset);
    CuAssertTrue(tc, 0 == memcmp(bin.data, snap, before));
    CuAssertTrue(tc, 0 == memcmp(bin.data + before, snap, before));
    binary_free(&bin);

    /* set_string：源是缓冲里那截自己刚写进去的字符串 */
    binary_ctx bs;
    char str[200];
    memset(str, 'y', sizeof(str) - 1);
    str[sizeof(str) - 1] = '\0';
    binary_init(&bs, NULL, 0, 0);
    binary_set_string(&bs, str);
    CuAssertTrue(tc, sizeof(str) == bs.offset);

    binary_set_string(&bs, bs.data);
    CuAssertTrue(tc, 2 * sizeof(str) == bs.offset);
    CuAssertTrue(tc, 0 == memcmp(bs.data, str, sizeof(str)));
    CuAssertTrue(tc, 0 == memcmp(bs.data + sizeof(str), str, sizeof(str)));
    binary_free(&bs);

    /* 源与目标真正重叠的一路——上面两段都先触发了 REALLOC，之后 src/dst 必然不相交，
     * 换回 memcpy 也照样通过。重叠要求 offset - aoff < lens，而"源全在已写区内"要求
     * offset - aoff >= lens，两者互斥；所以先写满再把写游标退回去，才能既不越界读又造出重叠。
     * 初始容量 256，写 200 后退到 100 再自追加 100 字节，不触发扩容 */
    binary_ctx bov;
    uint8_t seq[200];
    uint32_t k;
    binary_init(&bov, NULL, 0, 0);
    for (k = 0; k < sizeof(seq); k++) {
        seq[k] = (uint8_t)k;
    }
    binary_set_binary(&bov, (const char *)seq, sizeof(seq));
    binary_offset(&bov, 100);
    binary_set_binary(&bov, bov.data + 50, 100);/* dst [100,200) 与 src [50,150) 重叠 */
    CuAssertTrue(tc, 200 == bov.offset);
    /* memmove 语义：结果整段等于搬移前的 [50,150)。memcpy 正向拷会在后半段读到刚被自己
     * 覆盖过的字节，[150,200) 将变成 50..99 而不是 100..149，下面这条即可分辨 */
    for (k = 0; k < 100; k++) {
        CuAssertIntEquals(tc, (int32_t)(50 + k), (int32_t)(uint8_t)bov.data[100 + k]);
    }
    binary_free(&bov);
}

/* binary_remain / binary_have / binary_try_get_string：读之前的边界判定原语。
 * 报文长度由对端决定，binary_get_* 越界只会 ASSERTAB abort，这三个是唯一的预判手段 */
static void test_binary_bounds(CuTest *tc) {
    char raw[8] = { 'a', 'b', 'c', 0, 'd', 'e', 'f', 'g' };
    binary_ctx br;
    binary_init(&br, raw, sizeof(raw), 0);

    /* remain 随游标递减，读到底为 0 */
    CuAssertTrue(tc, sizeof(raw) == binary_remain(&br));
    binary_get_skip(&br, 3);
    CuAssertTrue(tc, sizeof(raw) - 3 == binary_remain(&br));

    /* have：恰好够 / 差一个 / 零剩余 */
    CuAssertTrue(tc, 0 != binary_have(&br, 5));
    CuAssertTrue(tc, 0 == binary_have(&br, 6));
    CuAssertTrue(tc, 0 != binary_have(&br, 0));
    binary_get_skip(&br, 5);
    CuAssertTrue(tc, 0 == binary_remain(&br));
    CuAssertTrue(tc, 0 != binary_have(&br, 0));
    CuAssertTrue(tc, 0 == binary_have(&br, 1));

    /* 形参是 uint64_t：超 32 位的长度不得被截断成小值蒙混过关（m32 构建上才有区别） */
    binary_offset(&br, 0);
    CuAssert(tc, "a 4GiB+1 length must never be accepted on an 8-byte buffer",
        0 == binary_have(&br, (uint64_t)0x100000001ULL));

    /* try_get_string：正常取 → 剩余段无 NUL 返 NULL 且不动游标 */
    binary_offset(&br, 0);
    char *s = binary_try_get_string(&br);
    CuAssertPtrNotNull(tc, s);
    CuAssertStrEquals(tc, "abc", s);
    CuAssertTrue(tc, 4 == br.offset);
    size_t before = br.offset;
    CuAssertTrue(tc, NULL == binary_try_get_string(&br)); /* "defg" 后面没有 NUL */
    CuAssertTrue(tc, before == br.offset);

    /* 空串是合法结果：只消耗那一个 NUL */
    char nul[1] = { 0 };
    binary_ctx bn;
    binary_init(&bn, nul, sizeof(nul), 0);
    char *e = binary_try_get_string(&bn);
    CuAssertPtrNotNull(tc, e);
    CuAssertTrue(tc, '\0' == e[0] && 1 == bn.offset);

    /* 零剩余同样返 NULL 而不是断言 */
    CuAssertTrue(tc, NULL == binary_try_get_string(&bn));
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

/* buffer_search 的范围守卫从前写作 start + wlens > end，start 接近 SIZE_MAX 时相加回绕、
 * 守卫失效，_buffer_search_start_cached 找不到起始节点后卡在 ASSERTAB 崩掉，
 * 而不是像 buffer_at / buffer_copyout 那样干净地拒掉越界 */
static void test_buffer_search_start_overflow(CuTest *tc) {
    buffer_ctx buf;
    const char *s = "Hello, World!";
    buffer_init(&buf);
    CuAssertTrue(tc, ERR_OK == buffer_append(&buf, (void *)s, strlen(s)));

    /* 正常查找不受影响 */
    CuAssertTrue(tc, 5 == buffer_search(&buf, 0, 0, 0, ", ", 2));
    /* 回绕值：改前在此 abort */
    CuAssertTrue(tc, ERR_FAILED == buffer_search(&buf, 0, (size_t)-1, 0, ", ", 2));
    CuAssertTrue(tc, ERR_FAILED == buffer_search(&buf, 0, (size_t)-2, 0, ", ", 2));
    /* 不回绕但同样越界的边界 */
    CuAssertTrue(tc, ERR_FAILED == buffer_search(&buf, 0, strlen(s), 0, ", ", 2));
    CuAssertTrue(tc, ERR_FAILED == buffer_search(&buf, 0, strlen(s) - 1, 0, ", ", 2));

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

/* sfid_id 取的是墙钟，时钟往回跳时它等时钟追上来。等待必须有上限：不设上限的话回拨多少
 * 就阻塞多少，一次 NTP 跳表能把调用线程卡住几十分钟。把 lasttimestamp 直接推到远未来
 * 模拟一次大幅回拨；预算调到 10ms，免得整套 C 测试为这一条白等一秒 */
static void test_sfid_clockback_giveup(CuTest *tc) {
    sfid_ctx ctx;
    CuAssertPtrNotNull(tc, sfid_init(&ctx, 1, 0, 0, 0));
    CuAssertIntEquals(tc, 1000, ctx.clockback_wait);/* sfid_init 的默认预算 */
    CuAssertTrue(tc, 0 != sfid_id(&ctx));

    ctx.clockback_wait = 10;
    ctx.lasttimestamp += 3600llu * 1000;/* 相当于时钟往回跳一小时 */
    CuAssertTrue(tc, 0 == sfid_id(&ctx));
    /* 预算是每次调用重新计的，第二次同样放弃而不是记住上次已超时 */
    CuAssertTrue(tc, 0 == sfid_id(&ctx));
}

/* 墙钟退到 customepoch 之前（NTP 步进 / 虚机快照恢复回旧日期）：nowms() - customepoch 无符号
 * 下溢成约 1.8e19 并被写进 lasttimestamp，此后时钟校正回来也永远追不上，该 ctx 从此只返 0。
 * sfid_init 挡不住这一档——它只在初始化那一刻校验 customepoch < now */
static void test_sfid_epoch_underflow(CuTest *tc) {
    sfid_ctx ctx;
    CuAssertPtrNotNull(tc, sfid_init(&ctx, 1, 0, 0, 0));
    CuAssertTrue(tc, 0 != sfid_id(&ctx));

    /* 把纪元直接推到未来，等价于墙钟退到纪元之前 */
    ctx.clockback_wait = 10;
    ctx.customepoch = nowms() + 3600llu * 1000;
    /* 下溢的话 curms 远大于 lasttimestamp，会走 else 分支"成功"返回一个垃圾 ID；
       钳到 0 后落进回拨分支，等满预算返 0 */
    CuAssertTrue(tc, 0 == sfid_id(&ctx));

    /* lasttimestamp 没被污染：纪元恢复后立刻又能正常出 ID */
    ctx.customepoch = 0;
    CuAssertTrue(tc, 0 != sfid_id(&ctx));
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

    /* is_loopback：v4 认整个 127.0.0.0/8 */
    CuAssertTrue(tc, ERR_OK == is_loopback("127.0.0.1"));
    CuAssertTrue(tc, ERR_OK == is_loopback("127.0.0.2"));
    CuAssertTrue(tc, ERR_OK == is_loopback("127.255.255.254"));
    CuAssertTrue(tc, ERR_OK != is_loopback("128.0.0.1"));
    CuAssertTrue(tc, ERR_OK != is_loopback("126.255.255.255"));
    CuAssertTrue(tc, ERR_OK != is_loopback("0.0.0.0"));
    CuAssertTrue(tc, ERR_OK != is_loopback("192.168.1.1"));
    /* is_loopback：v6 认 ::1 与 v4-mapped 的 127 段 */
    CuAssertTrue(tc, ERR_OK == is_loopback("::1"));
    CuAssertTrue(tc, ERR_OK == is_loopback("0:0:0:0:0:0:0:1"));
    CuAssertTrue(tc, ERR_OK == is_loopback("::ffff:127.0.0.1"));
    CuAssertTrue(tc, ERR_OK != is_loopback("::ffff:192.168.1.1"));
    CuAssertTrue(tc, ERR_OK != is_loopback("::"));
    CuAssertTrue(tc, ERR_OK != is_loopback("fe80::1"));
    /* is_loopback：主机名与非法串一律不认 */
    CuAssertTrue(tc, ERR_OK != is_loopback("localhost"));
    CuAssertTrue(tc, ERR_OK != is_loopback("not_an_ip"));
    CuAssertTrue(tc, ERR_OK != is_loopback(""));

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
    CuAssertIntEquals(tc, ERR_OK, sock_pair(fds, 1));

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
 * buffer —— search 带起始偏移、drain 超量时的边界行为。
 * 末两条 search 验空 needle 与 NULL needle：改前 wlens=0 会先越界读 what[0]，
 * 再因 _buffer_search_memcmp 的 while(wlen>0) 一次不执行而把首个等值字节报成命中（返 3）；
 * what=NULL 则直接解引用空指针
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

    pos = buffer_search(&buf, 0, 0, 0, "|", 0);
    CuAssertTrue(tc, ERR_FAILED == pos);
    pos = buffer_search(&buf, 0, 0, 0, NULL, 1);
    CuAssertTrue(tc, ERR_FAILED == pos);

    /* end 是闭区间：起点正好落在 end 上算命中，end 减 1 才排除掉 */
    CuAssertTrue(tc, 3 == buffer_search(&buf, 0, 0, 3, "|", 1));
    CuAssertTrue(tc, ERR_FAILED == buffer_search(&buf, 0, 0, 2, "|", 1));

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
 * buffer_external 节点的 misalign 区属调用方内存，任何写入路径都不得回收它。
 * 部分 drain 后 misalign>0 且 off>0，而 _buffer_should_realign 判的是
 * buffer_lens-off（不计 misalign），对外部节点恒为真：修复前 _buffer_align 会在
 * 调用方缓冲里 memmove，再把新数据 memcpy 进去，静默改写调用方仍要读的数据；
 * 若外部内存是只读映射则直接 SIGSEGV。
 * ext_free 传 NULL，同时覆盖"靠 _free 非空判定外部节点"会漏掉的情形
 * ======================================================================= */
static void test_buffer_external_not_writable(CuTest *tc) {
    buffer_ctx buf;
    char ext[1000];
    char readback[128];
    size_t i;

    memset(ext, 'E', sizeof(ext));
    buffer_init(&buf);
    buffer_external(&buf, ext, sizeof(ext), NULL);
    CuAssertTrue(tc, sizeof(ext) == buffer_size(&buf));
    buffer_drain(&buf, 900);
    CuAssertTrue(tc, 100 == buffer_size(&buf));

    CuAssertIntEquals(tc, ERR_OK, buffer_append(&buf, "APPENDED", 8));
    CuAssertTrue(tc, 108 == buffer_size(&buf));
    for (i = 0; i < sizeof(ext); i++) {
        CuAssertTrue(tc, 'E' == ext[i]);
    }
    CuAssertTrue(tc, 108 == buffer_copyout(&buf, 0, readback, 108));
    for (i = 0; i < 100; i++) {
        CuAssertTrue(tc, 'E' == readback[i]);
    }
    CuAssertTrue(tc, 0 == memcmp(readback + 100, "APPENDED", 8));
    buffer_free(&buf);

    memset(ext, 'E', sizeof(ext));
    buffer_init(&buf);
    buffer_external(&buf, ext, sizeof(ext), NULL);
    buffer_drain(&buf, 900);
    CuAssertIntEquals(tc, ERR_OK, buffer_appendv(&buf, "v=%d", 7));
    CuAssertTrue(tc, 103 == buffer_size(&buf));
    for (i = 0; i < sizeof(ext); i++) {
        CuAssertTrue(tc, 'E' == ext[i]);
    }
    buffer_free(&buf);
}

/* =======================================================================
 * 零长外部节点：buffer_external(lens=0) 不挂节点，data 即刻由 ext_free 归还。
 * 修复前该节点会挂上链，随后两条路径都会炸：
 *   写：_buffer_expand 给它记一条零长 iov，而 _buffer_commit_expand 的首节点跳过规则
 *       （空闲空间为 0 即跳）把同一节点跳过，iov 与节点逐位校验错位 → commit 返 0
 *       → buffer_append 的 ASSERTAB abort
 *   读：夹在两个数据节点之间时，_buffer_search_start_cached 的游走以 off!=0 为继续
 *       条件，在零长节点处提前终止，其后数据全部不可达 —— buffer_at / buffer_search
 *       abort，buffer_copyout(start>0) 静默返 0
 * ======================================================================= */
static void test_buffer_external_zero(CuTest *tc) {
    buffer_ctx buf;
    char readback[64];
    char *ext;

    // 1. lens=0 不挂节点，所有权已转移故 data 即刻归还
    ATOMIC_SET(&_ext_free_called, 0);
    buffer_init(&buf);
    MALLOC(ext, 8);
    buffer_external(&buf, ext, 0, _ext_free);
    CuAssertIntEquals(tc, 1, ATOMIC_GET(&_ext_free_called));
    CuAssertTrue(tc, 0 == buffer_size(&buf));

    // 2. 紧接着 append：修复前在此 abort "commit lens not equ buffer lens."
    CuAssertIntEquals(tc, ERR_OK, buffer_append(&buf, "x", 1));
    CuAssertTrue(tc, 1 == buffer_size(&buf));
    CuAssertTrue(tc, 1 == buffer_copyout(&buf, 0, readback, 1));
    CuAssertTrue(tc, 'x' == readback[0]);
    buffer_free(&buf);
    // 节点没挂上链，buffer_free 不应再次调用 ext_free
    CuAssertIntEquals(tc, 1, ATOMIC_GET(&_ext_free_called));

    // 3. 夹在两个数据节点之间：其后数据必须仍可 at / search / copyout
    buffer_init(&buf);
    CuAssertIntEquals(tc, ERR_OK, buffer_append(&buf, "0123456789", 10));
    ATOMIC_SET(&_ext_free_called, 0);
    MALLOC(ext, 8);
    buffer_external(&buf, ext, 0, _ext_free);
    CuAssertIntEquals(tc, 1, ATOMIC_GET(&_ext_free_called));
    char big[2000];
    memset(big, 'B', sizeof(big));
    big[sizeof(big) - 1] = 'Z';
    CuAssertIntEquals(tc, ERR_OK, buffer_append(&buf, big, sizeof(big)));
    CuAssertTrue(tc, 2010 == buffer_size(&buf));
    // 修复前 abort "index error."
    CuAssertTrue(tc, 'Z' == buffer_at(&buf, 2009));
    // 修复前 abort "can't search start node."
    CuAssertIntEquals(tc, 2009, buffer_search(&buf, 0, 1500, 0, "Z", 1));
    // 修复前静默返 0 丢数据
    CuAssertTrue(tc, 10 == buffer_copyout(&buf, 2000, readback, 10));
    CuAssertTrue(tc, 'Z' == readback[9]);
    buffer_free(&buf);
}

/* =======================================================================
 * buffer_get / buffer_commit_get 读暂存契约。
 * 补全前的问题是：buffer_get 不置 node->used，暂存期间一次 append 触发的对齐 memmove 或
 * _buffer_expand_single 节点迁移，就能让已暂存的 iov 指向被改写/已释放的内存
 * （freeze_read 只挡读接口，append/appendv 只断言 freeze_write）。
 * 现在：get 锁定节点、读写两族暂存态互斥（均以断言拦截）、commit_get 先解锁再 drain。
 * 本用例守的是"锁定与解锁配对"这半边——used 置了不清，drain 就会走"节点被锁定"分支，
 * 跨节点全量提交在 ASSERTAB(0 == remain) 处 abort。另半边（暂存期间的写入危害）
 * 现在被 append 的断言挡在门外，无法在用例里构造，故本用例不覆盖
 * ======================================================================= */
static void test_buffer_get_commit(CuTest *tc) {
    buffer_ctx buf;
    IOV_TYPE iov[MAX_EXPAND_NIOV];
    char big[2000];
    char readback[16];
    size_t total = 0;
    uint32_t n, i;

    memset(big, 'B', sizeof(big));
    buffer_init(&buf);

    // 空缓存返回 0 且不进入暂存态，故后续写接口不该被断言拦截
    CuAssertTrue(tc, 0 == buffer_get(&buf, 16, iov, MAX_EXPAND_NIOV));
    CuAssertIntEquals(tc, ERR_OK, buffer_append(&buf, "0123456789", 10));

    // 第二次 append 放不进首节点余量，必然跨出第二个节点
    CuAssertIntEquals(tc, ERR_OK, buffer_append(&buf, big, sizeof(big)));
    CuAssertTrue(tc, 2010 == buffer_size(&buf));

    // 暂存全部数据：iov 按节点切分，总长应等于 buffer_size
    n = buffer_get(&buf, buffer_size(&buf), iov, MAX_EXPAND_NIOV);
    CuAssertTrue(tc, n >= 2);
    for (i = 0; i < n; i++) {
        total += (size_t)iov[i].IOV_LEN_FIELD;
    }
    CuAssertTrue(tc, 2010 == total);
    CuAssertTrue(tc, 0 == memcmp(iov[0].IOV_PTR_FIELD, "0123456789", 10));

    // 解除暂存并全量删除：补全前在 drain 的 ASSERTAB 处 abort
    buffer_commit_get(&buf, 2010);
    CuAssertTrue(tc, 0 == buffer_size(&buf));

    // 节点是真被释放而非留成零长节点，重新写入可正常读回
    CuAssertIntEquals(tc, ERR_OK, buffer_append(&buf, "after", 5));
    CuAssertTrue(tc, 5 == buffer_size(&buf));
    CuAssertTrue(tc, 5 == buffer_copyout(&buf, 0, readback, 5));
    CuAssertTrue(tc, 0 == memcmp(readback, "after", 5));
    buffer_free(&buf);

    // 部分提交：只删一部分，剩余数据仍可读且内容正确
    buffer_init(&buf);
    CuAssertIntEquals(tc, ERR_OK, buffer_append(&buf, "0123456789", 10));
    CuAssertTrue(tc, 1 == buffer_get(&buf, 10, iov, MAX_EXPAND_NIOV));
    buffer_commit_get(&buf, 4);
    CuAssertTrue(tc, 6 == buffer_size(&buf));
    CuAssertTrue(tc, 6 == buffer_copyout(&buf, 0, readback, 6));
    CuAssertTrue(tc, 0 == memcmp(readback, "456789", 6));

    // 暂存后只解锁不删除，数据应原样留下
    CuAssertTrue(tc, 1 == buffer_get(&buf, 6, iov, MAX_EXPAND_NIOV));
    buffer_commit_get(&buf, 0);
    CuAssertTrue(tc, 6 == buffer_size(&buf));
    CuAssertIntEquals(tc, ERR_OK, buffer_append(&buf, "Z", 1));
    CuAssertTrue(tc, 7 == buffer_size(&buf));
    buffer_free(&buf);
}

/* =======================================================================
 * buffer_space：报出"不新建节点就能写入"的字节数。
 * 契约是它与 buffer_expand 的登记规则严格对齐 —— 照它报的量写满，
 * 可写空间应恰好归零，且中途不该冒出新节点
 * ======================================================================= */
static void test_buffer_space(CuTest *tc) {
    buffer_ctx buf;
    char *fill;
    size_t sp;

    buffer_init(&buf);
    // 空链没有任何可写空间
    CuAssertTrue(tc, 0 == buffer_space(&buf, MAX_EXPAND_NIOV));

    // 首节点按 1KB 对齐分配，写 3 字节后必然还剩余量
    CuAssertIntEquals(tc, ERR_OK, buffer_append(&buf, "abc", 3));
    sp = buffer_space(&buf, MAX_EXPAND_NIOV);
    CuAssertTrue(tc, sp > 0);

    // 照报出的量正好写满：空间归零说明既没少报（否则还有剩）也没多报（否则会新建节点）
    MALLOC(fill, sp);
    memset(fill, 'F', sp);
    CuAssertIntEquals(tc, ERR_OK, buffer_append(&buf, fill, sp));
    FREE(fill);
    CuAssertTrue(tc, (3 + sp) == buffer_size(&buf));
    CuAssertTrue(tc, 0 == buffer_space(&buf, MAX_EXPAND_NIOV));
    buffer_free(&buf);
}

/* =======================================================================
 * buffer_from_sock 的读循环不再为确认性 readv 白分配节点。
 * epoll 是 EPOLLET，必须读到无数据为止，所以短读后那一轮 readv 省不掉；
 * 但它大概率直接 EAGAIN，没必要为它再要一个 MAX_RECV_SIZE 的新节点 ——
 * 改为只用 buffer_space 报出的现成余量。读满的那轮说明还有数据，仍按
 * MAX_RECV_SIZE 取，否则大流量下每轮只读几百字节，readv 次数翻倍
 * ======================================================================= */
#define FAKE_RV_MAX 3// 场景二是最长的一路: 两轮读满 + 一轮确认
static size_t _fake_rv_want[FAKE_RV_MAX];// 第 i 次调用要吐出的字节数
static size_t _fake_rv_offer[FAKE_RV_MAX];// 第 i 次调用被提供的 iov 总空间
static int32_t _fake_rv_calls;
// 假 readv：无需真 socket 即可驱动 buffer_from_sock 的 ET 读循环
static int32_t _fake_readv(SOCKET fd, IOV_TYPE *iov, uint32_t niov, void *arg, size_t *readed) {
    (void)fd;
    (void)arg;
    size_t offer = 0;
    size_t remain = 0;
    size_t n;
    uint32_t i;
    for (i = 0; i < niov; i++) {
        offer += (size_t)iov[i].IOV_LEN_FIELD;
    }
    if (_fake_rv_calls < FAKE_RV_MAX) {
        _fake_rv_offer[_fake_rv_calls] = offer;
        remain = _fake_rv_want[_fake_rv_calls];
    }
    _fake_rv_calls++;
    if (remain > offer) {
        remain = offer;
    }
    *readed = 0;
    for (i = 0; i < niov && remain > 0; i++) {
        n = (size_t)iov[i].IOV_LEN_FIELD;
        if (n > remain) {
            n = remain;
        }
        memset(iov[i].IOV_PTR_FIELD, 'R', n);
        remain -= n;
        *readed += n;
    }
    return ERR_OK;
}
static void _fake_rv_reset(void) {
    memset(_fake_rv_want, 0, sizeof(_fake_rv_want));
    memset(_fake_rv_offer, 0, sizeof(_fake_rv_offer));
    _fake_rv_calls = 0;
}
static void test_buffer_from_sock_space(CuTest *tc) {
#ifdef READV_EINVAL
    // AIX 的 readv 无数据时返回 EINVAL，buffer_from_sock 在自适应 nbuf 那段之前就 break，
    // 被测逻辑在该平台是死代码，下面断言的 readv 轮次与总字节数也都对不上，整体跳过
    (void)tc;
#else
    buffer_ctx buf;
    size_t nread;

    // 场景一：一次短读后 EAGAIN
    buffer_init(&buf);
    _fake_rv_reset();
    _fake_rv_want[0] = 2000;
    CuAssertIntEquals(tc, ERR_OK, buffer_from_sock(&buf, 0, &nread, _fake_readv, NULL));
    CuAssertTrue(tc, 2000 == nread);
    CuAssertTrue(tc, 2000 == buffer_size(&buf));
    // 确认轮确实发生了 —— 不能靠"不读"来省开销，那会违反 ET 契约
    CuAssertIntEquals(tc, 2, _fake_rv_calls);
    // 首轮无历史可依，仍按 MAX_RECV_SIZE 要空间
    CuAssertTrue(tc, _fake_rv_offer[0] >= MAX_RECV_SIZE);
    // 确认轮只拿首节点写剩的余量，不为它新建节点 —— 这一条才是被测行为本身
    CuAssertTrue(tc, _fake_rv_offer[1] < MAX_RECV_SIZE);
    // 首节点是为 MAX_RECV_SIZE 建的，写掉 2000 后余量必然小于 MAX_RECV_SIZE；
    // 若确认轮又建了一个空节点，可写空间会被顶到 MAX_RECV_SIZE 以上
    CuAssertTrue(tc, buffer_space(&buf, MAX_EXPAND_NIOV) < MAX_RECV_SIZE);
    buffer_free(&buf);

    // 场景二：连续两轮读满，后续轮次仍须按 MAX_RECV_SIZE 要空间
    buffer_init(&buf);
    _fake_rv_reset();
    _fake_rv_want[0] = MAX_RECV_SIZE;
    _fake_rv_want[1] = MAX_RECV_SIZE;
    CuAssertIntEquals(tc, ERR_OK, buffer_from_sock(&buf, 0, &nread, _fake_readv, NULL));
    CuAssertTrue(tc, (2 * MAX_RECV_SIZE) == nread);
    CuAssertTrue(tc, (2 * MAX_RECV_SIZE) == buffer_size(&buf));
    CuAssertIntEquals(tc, 3, _fake_rv_calls);
    CuAssertTrue(tc, _fake_rv_offer[0] >= MAX_RECV_SIZE);
    CuAssertTrue(tc, _fake_rv_offer[1] >= MAX_RECV_SIZE);
    CuAssertTrue(tc, _fake_rv_offer[2] >= MAX_RECV_SIZE);
    buffer_free(&buf);
#endif
}

/* =======================================================================
 * buffer_free 复位语义：释放后 ctx 回到 buffer_init 后的空状态。
 * 修复前只释放节点链，head/tail/tail_with_data/hint_node/total_lens 全留陈旧值：
 * 二次调用是 double free，buffer_size 报释放前的字节数，copyout 从已释放节点 memcpy
 * ======================================================================= */
static void test_buffer_free_resets(CuTest *tc) {
    buffer_ctx buf;
    char readback[64];

    buffer_init(&buf);
    CuAssertIntEquals(tc, ERR_OK, buffer_append(&buf, "hello world", 11));
    CuAssertTrue(tc, 11 == buffer_size(&buf));
    /* start>0 的 copyout 会把游标缓存进 hint_node，确保释放时该字段也指向节点 */
    CuAssertTrue(tc, 5 == buffer_copyout(&buf, 6, readback, 5));
    CuAssertTrue(tc, 0 == memcmp("world", readback, 5));

    buffer_free(&buf);
    CuAssertTrue(tc, 0 == buffer_size(&buf));
    /* 修复前 total_lens 仍是 11，这里会从已释放的 head 节点 memcpy 出 8 字节 */
    CuAssertTrue(tc, 0 == buffer_copyout(&buf, 0, readback, 8));

    /* 重复释放安全 */
    buffer_free(&buf);
    CuAssertTrue(tc, 0 == buffer_size(&buf));

    /* 释放后可继续当空 buffer 复用 */
    CuAssertIntEquals(tc, ERR_OK, buffer_append(&buf, "again", 5));
    CuAssertTrue(tc, 5 == buffer_size(&buf));
    CuAssertTrue(tc, 5 == buffer_copyout(&buf, 0, readback, 5));
    CuAssertTrue(tc, 0 == memcmp("again", readback, 5));
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
    CuAssertTrue(tc, 0 == memcmp(span, seed + 580, 20));// [580,600) 原始尾
    CuAssertTrue(tc, 0 == memcmp(span + 20, big, 40));// [600,640) 追加头
    buffer_free(&buf);
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
 * hug —— 退出等待原语(POSIX self-pipe / Windows mutex+cond)
 * ======================================================================= */
// hug_wait 阻塞在 read 上时由另一条线程唤醒,走真正的 self-pipe 路径
static void _hug_waker(void *arg) {
    MSLEEP(50);
    hug_wakeup((hug_ctx *)arg);
}

static void test_hug(CuTest *tc) {
    hug_ctx hug;

    // 1) 先 wakeup 再 wait: 标记已置位, hug_wait 不进 read 直接返回
    CuAssertTrue(tc, ERR_OK == hug_init(&hug));
    hug_wakeup(&hug);
    hug_wait(&hug);
    hug_free(&hug);

    // 2) 先 wait 再由别的线程 wakeup: 真正阻塞在 read 上被唤醒
    // (若 waker 抢先跑完, hug_wait 直接返回, 同样通过, 不构成 flake)
    CuAssertTrue(tc, ERR_OK == hug_init(&hug));
    pthread_t tid = thread_creat(_hug_waker, &hug);
    hug_wait(&hug);
    thread_join(tid);
    hug_free(&hug);
}

// 信号可反复投递, 而 hug_wait 返回后再没人读管道。若每次 hug_wakeup 都写一个字节,
// 写满 pipe 容量(Linux/FreeBSD 65536, macOS 16384)之后 write 会阻塞在信号 handler 里,
// 收到信号那条线程再也回不来。修复后只有首次真正写管道, 故刷 20 万次也不会卡
static void test_hug_wakeup_flood(CuTest *tc) {
    hug_ctx hug;
    CuAssertTrue(tc, ERR_OK == hug_init(&hug));

    for (int32_t i = 0; i < 200000; i++) {
        hug_wakeup(&hug);
    }
#ifndef OS_WIN
    // 直接验不变式而不只是"没卡住": 管道里应当恰好剩 1 个字节
    (void)fcntl(hug.exit_pipe[0], F_SETFL, O_NONBLOCK);
    char drain[64];
    ssize_t total = 0;
    ssize_t n;
    while ((n = read(hug.exit_pipe[0], drain, sizeof(drain))) > 0) {
        total += n;
    }
    CuAssertTrue(tc, 1 == total);
#endif
    hug_wait(&hug);
    hug_free(&hug);
}

/* =======================================================================
 * timeofday —— 现已由 _now_usec 推导，须与 nowsec / nowms 同源。
 * 三者取自同一次 gettimeofday/GetSystemTimeAsFileTime 换算，跨秒边界允许差 1 秒
 * ======================================================================= */
static void test_timeofday_consistent(CuTest *tc) {
    struct timeval tv;
    uint64_t sec, ms;

    timeofday(&tv);
    sec = nowsec();
    ms = nowms();
    CuAssertTrue(tc, tv.tv_sec > 0);
    CuAssertTrue(tc, tv.tv_usec >= 0 && tv.tv_usec < 1000000);
    CuAssertTrue(tc, (uint64_t)tv.tv_sec <= sec && sec - (uint64_t)tv.tv_sec <= 1);
    CuAssertTrue(tc, ms / 1000 == sec || ms / 1000 == sec - 1 || ms / 1000 == sec + 1);
}

/* =======================================================================
 * timer —— 初始化、当前时刻、计时。
 * fresh 那两行验的是 timer_init 已把计时起点置为当前时刻：
 * 未调 timer_start 就读 elapsed 也须是小值，而不是未初始化的 starttick 垃圾
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

    timer_ctx fresh;
    timer_init(&fresh);
    CuAssertTrue(tc, timer_elapsed_ms(&fresh) < 1000);
    CuAssertTrue(tc, timer_elapsed(&fresh) < 1000ULL * 1000ULL * 1000ULL);

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

    /* 与 time() 这个独立时间源交叉校验：两者同源坏掉时"量级一致"仍成立，挡不住。
       Windows 上若经 struct timeval 的 32 位 tv_sec，2038 后符号扩展成约 1.8e19，
       与 time() 差十几个数量级。±2s 容差覆盖跨秒边界 */
    uint64_t tsec = (uint64_t)time(NULL);
    CuAssertTrue(tc, sec + 2 >= tsec && tsec + 2 >= sec);
    CuAssertTrue(tc, ms / 1000 + 2 >= tsec && tsec + 2 >= ms / 1000);

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

    /* %z 只吃掉时区文本、偏移量不生效（TM_GMTOFF 全仓未定义），所以同一时刻配不同偏移
     * 必然解析成同一个时间戳。这是 strtots 声明处所述行为的守卫：哪天真把偏移接上，
     * 本断言会失败并提醒同步改文档；用相等而非绝对值断言，不受进程时区影响 */
    uint64_t z0 = strtots("2026-06-15 12:00:00+0000", "%Y-%m-%d %H:%M:%S%z");
    uint64_t z8 = strtots("2026-06-15 12:00:00+0800", "%Y-%m-%d %H:%M:%S%z");
    CuAssertTrue(tc, z0 > 0 && z8 > 0);
    CuAssertTrue(tc, z0 == z8);
    CuAssertTrue(tc, z0 == t0);
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
    n = popen_read(&ctx, buf, sizeof(buf) - 1, NULL);
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
    n = popen_read(&ctx, buf, sizeof(buf) - 1, NULL);
    CuAssertTrue(tc, n > 0);
    CuAssertTrue(tc, NULL != strstr(buf, "srey test"));
    popen_free(&ctx);

    CuAssertIntEquals(tc, ERR_OK, popen_startup(&ctx, cmd, "rw"));
    n = popen_write(&ctx, msg, strlen(msg));
    CuAssertTrue(tc, n > 0);
    CuAssertIntEquals(tc, ERR_OK, popen_waitexit(&ctx, 3000));
    popen_free(&ctx);
    popen_free(&ctx);
    CuAssert(tc, "popen_free idempotent: double free must not close the handle twice",
        ERR_FAILED == popen_read(&ctx, buf, sizeof(buf) - 1, NULL));
    CuAssert(tc, "popen_free idempotent: write after free must fail, not write a closed handle",
        ERR_FAILED == popen_write(&ctx, "x", 1));

    /* 子进程不读 stdin 时的大块写：写端非阻塞，写满对端缓冲即返回已写字节数。
       回归表现为整个测试进程挂在 write 上不返回（socketpair 发送缓冲通常只有几十 KB，
       而这里写 1 MiB），生产环境下卡住的是派发该调用的 worker 线程 */
#ifndef OS_WIN
    /* 没调过 popen_waitexit 就直接 popen_close：子进程早已自己正常退完，
       kill 打在僵尸上无效果，waitpid 拿到的是真实退出码。
       原先无条件写 ERR_FAILED，会把 exit(7) 报成 -1。
       先读到 EOF 再 close：EOF 意味着子进程已关掉它那端(即已 _exit)，退出状态此刻已定死，
       随后的 SIGKILL 改不了它，所以不靠 sleep 也是确定的 */
    CuAssertIntEquals(tc, ERR_OK, popen_startup(&ctx, "exit 7", "r"));
    int32_t eof = 0;
    int32_t spin = 0;
    while (0 == eof && spin++ < 3000) {
        n = popen_read(&ctx, buf, sizeof(buf) - 1, &eof);
        if (ERR_FAILED == n) {
            break;
        }
        if (0 == n && 0 == eof) {
            MSLEEP(1);
        }
    }
    CuAssertIntEquals(tc, 1, eof);
    popen_close(&ctx);
    CuAssertIntEquals(tc, 7, popen_exitcode(&ctx));
    popen_free(&ctx);

    CuAssertIntEquals(tc, ERR_OK, popen_startup(&ctx, "sleep 30", "w"));
    size_t big = ONEK * ONEK;
    char *payload;
    MALLOC(payload, big);
    memset(payload, 'x', big);
    n = popen_write(&ctx, payload, big);
    FREE(payload);
    CuAssertTrue(tc, n > 0);
    CuAssertTrue(tc, (size_t)n < big);// 一次写不完，返回部分
    // 必须 close 再 free：kill + waitpid 在 close 里，free 只关 socket。
    // 少这一句，sleep 30 会活过本用例成为没人回收的子进程，而 ./bin/test 要阻塞等 SIGINT，
    // 它就一直挂到整个会话结束，每跑一轮再漏一个
    popen_close(&ctx);
    popen_free(&ctx);
#endif
}

/* =======================================================================
 * log —— 日志等级 set/get
 * ======================================================================= */
static void test_log_lv(CuTest *tc) {
    /* 注：main.c 已 log_init(NULL, 0)，此处仅做 set/get 一致性验证，
     * 测试结束后还原默认级别避免影响后续日志输出 */
    log_level prev = log_getlv();

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
    log_level prev = log_getlv();
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

    /* 格式不带 %a/%A/%w/%u 时由 y/m/d 推 tm_wday，须与 mktime 一致 */
    struct tm ref;
    // 不放 1970-01-01：MSVC 的 mktime 域从 1970-01-01T00:00:00Z 起，UTC+ 时区下本地午夜换算成
    // 1969-12-31 会返 -1，而 glibc/BSD 接受负 time_t，本机看不出来
    const char *days[] = { "2024-05-21", "2000-01-01", "1999-12-31", "2026-08-30" };
    for (size_t d = 0; d < sizeof(days) / sizeof(days[0]); d++) {
        ZERO(&tm, sizeof(tm));
        CuAssertPtrNotNull(tc, _strptime(days[d], "%Y-%m-%d", &tm));
        ZERO(&ref, sizeof(ref));
        ref.tm_year = tm.tm_year;
        ref.tm_mon = tm.tm_mon;
        ref.tm_mday = tm.tm_mday;
        ref.tm_isdst = -1;
        CuAssertTrue(tc, (time_t)-1 != mktime(&ref));
        CuAssertIntEquals(tc, ref.tm_wday, tm.tm_wday);
    }

    /* %D 是 POSIX 的 "%m/%d/%y"，不是 %x 的 "%y/%m/%d" */
    ZERO(&tm, sizeof(tm));
    CuAssertPtrNotNull(tc, _strptime("05/21/24", "%D", &tm));
    CuAssertIntEquals(tc, 2024 - 1900, tm.tm_year);
    CuAssertIntEquals(tc, 5 - 1,       tm.tm_mon);
    CuAssertIntEquals(tc, 21,          tm.tm_mday);
    /* %x 保持本实现的年在前 */
    ZERO(&tm, sizeof(tm));
    CuAssertPtrNotNull(tc, _strptime("24/05/21", "%x", &tm));
    CuAssertIntEquals(tc, 2024 - 1900, tm.tm_year);
    CuAssertIntEquals(tc, 5 - 1,       tm.tm_mon);
    CuAssertIntEquals(tc, 21,          tm.tm_mday);

    /* %b 匹配不上时不得带着未写过的 tm_mon 去跑收尾段(曾越界索引 start_of_month) */
    ZERO(&tm, sizeof(tm));
    tm.tm_mon = 100000;
    CuAssertTrue(tc, NULL == _strptime("2024 05 Xyz", "%Y %d %b", &tm));
    /* 同理 %a */
    ZERO(&tm, sizeof(tm));
    tm.tm_wday = 100000;
    CuAssertTrue(tc, NULL == _strptime("2024-05-21 Xyz", "%Y-%m-%d %a", &tm));
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
 * memichr / memstr / trim —— 字符/子串查找与两端剔除
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

    /* trim_left / trim_right / trim：仅剔 SP 与 HTAB，不改源数据、不写 '\0' */
    char tbuf[] = " \t abc \t ";
    size_t tlen = sizeof(tbuf) - 1;
    size_t n = 0;
    char *r = trim_left(tbuf, tlen, &n);
    CuAssertPtrNotNull(tc, r);
    CuAssert(tc, "trim_left start jumps to first non-whitespace", 'a' == r[0]);
    CuAssert(tc, "trim_left strips left only, trailing whitespace still counted", strlen("abc \t ") == n);
    r = trim_right(tbuf, tlen, &n);
    CuAssertPtrNotNull(tc, r);
    CuAssert(tc, "trim_right start unchanged", r == tbuf);
    CuAssert(tc, "trim_right strips right only", strlen(" \t abc") == n);
    r = trim(tbuf, tlen, &n);
    CuAssertPtrNotNull(tc, r);
    CuAssert(tc, "trim strips both ends", 3 == n && 0 == memcmp(r, "abc", 3));
    CuAssert(tc, "trim does not modify the source", 0 == memcmp(tbuf, " \t abc \t ", tlen));
    /* 全为空白 → 三者均返回 NULL 且写 0 */
    char wbuf[] = " \t \t";
    n = 99;
    CuAssert(tc, "all-whitespace trim_left returns NULL", NULL == trim_left(wbuf, sizeof(wbuf) - 1, &n) && 0 == n);
    n = 99;
    CuAssert(tc, "all-whitespace trim_right returns NULL", NULL == trim_right(wbuf, sizeof(wbuf) - 1, &n) && 0 == n);
    n = 99;
    CuAssert(tc, "all-whitespace trim returns NULL", NULL == trim(wbuf, sizeof(wbuf) - 1, &n) && 0 == n);
    /* 零长度输入 */
    n = 99;
    CuAssert(tc, "zero-length trim returns NULL", NULL == trim(tbuf, 0, &n) && 0 == n);
    /* 无空白：起点与长度都不变 */
    char pbuf[] = "abc";
    r = trim(pbuf, 3, &n);
    CuAssert(tc, "trim returns input as-is when there is no whitespace", r == pbuf && 3 == n);
}

/* =======================================================================
 * strupper / strlower / strreverse / tohex / split
 * ======================================================================= */
// safe_fill_str 的契约：装得下才写，装不下一个字节都不动并返回 ERR_FAILED
static void test_safe_fill_str(CuTest *tc) {
    char buf[8];
    memset(buf, 'Z', sizeof(buf));

    // 正好填满（容量 - 1）
    CuAssertIntEquals(tc, ERR_OK, safe_fill_str(buf, sizeof(buf), "1234567"));
    CuAssertStrEquals(tc, "1234567", buf);
    // 再多一个字节就拒绝，且 buf 保持上一次的内容不动
    CuAssertIntEquals(tc, ERR_FAILED, safe_fill_str(buf, sizeof(buf), "12345678"));
    CuAssertStrEquals(tc, "1234567", buf);
    // NULL 视为空串，算成功
    CuAssertIntEquals(tc, ERR_OK, safe_fill_str(buf, sizeof(buf), NULL));
    CuAssertStrEquals(tc, "", buf);
    // 空串
    CuAssertIntEquals(tc, ERR_OK, safe_fill_str(buf, sizeof(buf), ""));
    CuAssertStrEquals(tc, "", buf);
    // dstsz 为 0：不写不越界，直接失败
    CuAssertIntEquals(tc, ERR_FAILED, safe_fill_str(buf, 0, "x"));
}
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
    CuAssertTrue(tc, hex == tohex(bin, 4, hex, 0));
    CuAssertStrEquals(tc, "00ABFF10", hex);
    /* tohex：二进制转 16 进制（小写）*/
    char hexlower[HEX_ENSIZE(4)];
    hexlower[HEX_ENSIZE(4) - 1] = '\0';
    CuAssertTrue(tc, hexlower == tohex(bin, 4, hexlower, 1));
    CuAssertStrEquals(tc, "00abff10", hexlower);

    /* fromhex：大小写都认，非十六进制字符自己判得出来，不必外面先 isxdigit */
    CuAssertIntEquals(tc, 0, fromhex('0'));
    CuAssertIntEquals(tc, 9, fromhex('9'));
    CuAssertIntEquals(tc, 10, fromhex('a'));
    CuAssertIntEquals(tc, 15, fromhex('f'));
    CuAssertIntEquals(tc, 10, fromhex('A'));
    CuAssertIntEquals(tc, 15, fromhex('F'));
    CuAssertIntEquals(tc, ERR_FAILED, fromhex('g'));
    CuAssertIntEquals(tc, ERR_FAILED, fromhex('G'));
    CuAssertIntEquals(tc, ERR_FAILED, fromhex('/'));
    CuAssertIntEquals(tc, ERR_FAILED, fromhex(':'));
    CuAssertIntEquals(tc, ERR_FAILED, fromhex('\0'));
    /* tohex 的输出必须能被 fromhex 逐字符还原回去 */
    for (size_t i = 0; i < 4; i++) {
        CuAssertIntEquals(tc, bin[i], fromhex(hex[i * 2]) * 16 + fromhex(hex[i * 2 + 1]));
    }

    /* 堆模式(cap 为 0)：以 "," 拆分，段数即返回值 */
    char heapin[] = "aa,bb,cc";
    buf_ctx *parts = NULL;
    int32_t n = split(heapin, strlen(heapin), ",", 1, &parts, 0, 0);
    CuAssertIntEquals(tc, 3, n);
    CuAssertPtrNotNull(tc, parts);
    CuAssertTrue(tc, 2 == parts[0].lens && 0 == memcmp(parts[0].data, "aa", 2));
    CuAssertTrue(tc, 2 == parts[1].lens && 0 == memcmp(parts[1].data, "bb", 2));
    CuAssertTrue(tc, 2 == parts[2].lens && 0 == memcmp(parts[2].data, "cc", 2));
    FREE(parts);

    /* 堆模式：分隔符在尾部 → 补空段（段数 = sep 出现次数 + 1） */
    char heaptail[] = "x,y,";
    parts = NULL;
    n = split(heaptail, strlen(heaptail), ",", 1, &parts, 0, 0);
    CuAssertIntEquals(tc, 3, n);
    CuAssertIntEquals(tc, 0, (int32_t)parts[2].lens);
    FREE(parts);

    /* 堆模式：sep 为 NULL → 整段不切 */
    char heapone[] = "hello";
    parts = NULL;
    n = split(heapone, strlen(heapone), NULL, 0, &parts, 0, 0);
    CuAssertIntEquals(tc, 1, n);
    CuAssertTrue(tc, 5 == parts[0].lens);
    FREE(parts);

    /* 多字节分隔符：合并前只有堆那侧支持，现在栈模式一样能用 */
    char mbin[] = "a::b::c";
    buf_ctx mbsegs[4];
    buf_ctx *pmb = mbsegs;
    n = split(mbin, strlen(mbin), "::", 2, &pmb, 4, 0);
    CuAssertIntEquals(tc, 3, n);
    CuAssertTrue(tc, 1 == mbsegs[0].lens && 0 == memcmp(mbsegs[0].data, "a", 1));
    CuAssertTrue(tc, 1 == mbsegs[2].lens && 0 == memcmp(mbsegs[2].data, "c", 1));

    /* 参数非法一律 ERR_FAILED，且不碰 segs */
    parts = NULL;
    CuAssertIntEquals(tc, ERR_FAILED, split(NULL, 5, ",", 1, &parts, 0, 0));
    CuAssertPtrEquals(tc, NULL, parts);
    /* plens 为 0 不是错:出一个空段(url_parse 解 "/" 依赖这条) */
    parts = NULL;
    CuAssertIntEquals(tc, 1, split(heapin, 0, ",", 1, &parts, 0, 0));
    CuAssertIntEquals(tc, 0, (int32_t)parts[0].lens);
    FREE(parts);
    parts = NULL;
    CuAssertIntEquals(tc, ERR_FAILED, split(heapin, 3, ",", 1, NULL, 0, 0));
    CuAssertIntEquals(tc, ERR_FAILED, split(heapin, 3, ",", 1, &parts, -1, 0));
    buf_ctx *pnull = NULL;/* 栈模式却没给数组 */
    CuAssertIntEquals(tc, ERR_FAILED, split(heapin, 3, ",", 1, &pnull, 4, 0));

    /* 栈模式(cap 大于 0)：保留空段，段数 = sep 数 + 1 */
    buf_ctx segs[8];
    buf_ctx *psegs = segs;
    char sp1[] = "a/b/c";
    int32_t sn = split(sp1, strlen(sp1), "/", 1, &psegs, 8, 0);
    CuAssertIntEquals(tc, 3, sn);
    CuAssertTrue(tc, 1 == segs[0].lens && 0 == memcmp(segs[0].data, "a", 1));
    CuAssertTrue(tc, 1 == segs[2].lens && 0 == memcmp(segs[2].data, "c", 1));

    /* 栈模式：尾随/连续 sep 产生 len==0 空段，data 记原指针不是 NULL */
    char sp2[] = "a//";
    sn = split(sp2, strlen(sp2), "/", 1, &psegs, 8, 0);
    CuAssertIntEquals(tc, 3, sn);
    CuAssertIntEquals(tc, 0, (int32_t)segs[1].lens);
    CuAssertIntEquals(tc, 0, (int32_t)segs[2].lens);
    CuAssertPtrNotNull(tc, segs[1].data);

    /* 栈模式：段数超 cap 返回 ERR_FAILED */
    char sp3[] = "a/b/c/d";
    CuAssertIntEquals(tc, ERR_FAILED, split(sp3, strlen(sp3), "/", 1, &psegs, 2, 0));

    /* SPLIT_TRIM：每段剔两端 OWS，段数不变 */
    char sp4[] = " a , b ,c";
    sn = split(sp4, strlen(sp4), ",", 1, &psegs, 8, SPLIT_TRIM);
    CuAssertIntEquals(tc, 3, sn);
    CuAssertTrue(tc, 1 == segs[0].lens && 0 == memcmp(segs[0].data, "a", 1));
    CuAssertTrue(tc, 1 == segs[1].lens && 0 == memcmp(segs[1].data, "b", 1));
    CuAssertTrue(tc, 1 == segs[2].lens && 0 == memcmp(segs[2].data, "c", 1));

    /* SPLIT_SKIPEMPTY：只丢真正的空段，全空白段长度非 0 仍保留 */
    char sp5[] = "a,, ,b";
    sn = split(sp5, strlen(sp5), ",", 1, &psegs, 8, SPLIT_SKIPEMPTY);
    CuAssertIntEquals(tc, 3, sn);
    CuAssertIntEquals(tc, 1, (int32_t)segs[1].lens);/* " " 未 trim，不算空 */

    /* TRIM|SKIPEMPTY：trim 必须排在判空之前，全空白段这时才被丢掉 */
    char sp6[] = "a,, ,b";
    sn = split(sp6, strlen(sp6), ",", 1, &psegs, 8, SPLIT_TRIM | SPLIT_SKIPEMPTY);
    CuAssertIntEquals(tc, 2, sn);
    CuAssertTrue(tc, 1 == segs[0].lens && 0 == memcmp(segs[0].data, "a", 1));
    CuAssertTrue(tc, 1 == segs[1].lens && 0 == memcmp(segs[1].data, "b", 1));

    /* cap 判定排在过滤之后，被丢掉的空段不占名额 */
    char sp7[] = "a,,,,,,,,b";
    sn = split(sp7, strlen(sp7), ",", 1, &psegs, 2, SPLIT_TRIM | SPLIT_SKIPEMPTY);
    CuAssertIntEquals(tc, 2, sn);

    /* SPLIT_TRUNCATE：超 cap 截断到 cap 正常返回，不再 ERR_FAILED */
    char sp8[] = "a/b/c/d";
    sn = split(sp8, strlen(sp8), "/", 1, &psegs, 2, SPLIT_TRUNCATE);
    CuAssertIntEquals(tc, 2, sn);
    CuAssertTrue(tc, 1 == segs[0].lens && 0 == memcmp(segs[0].data, "a", 1));
    CuAssertTrue(tc, 1 == segs[1].lens && 0 == memcmp(segs[1].data, "b", 1));

    /* 堆模式同一套标志：" a , , b " → 两段 */
    char str3[] = " a , , b ";
    parts = NULL;
    n = split(str3, strlen(str3), ",", 1, &parts, 0, SPLIT_TRIM | SPLIT_SKIPEMPTY);
    CuAssertIntEquals(tc, 2, n);
    CuAssertTrue(tc, 1 == parts[0].lens && 0 == memcmp(parts[0].data, "a", 1));
    CuAssertTrue(tc, 1 == parts[1].lens && 0 == memcmp(parts[1].data, "b", 1));
    FREE(parts);

    /* 堆模式全被丢光：返回 0，但 *segs 已分配，仍须 FREE（契约写明的那一档） */
    char str4[] = " , , ";
    parts = NULL;
    n = split(str4, strlen(str4), ",", 1, &parts, 0, SPLIT_TRIM | SPLIT_SKIPEMPTY);
    CuAssertIntEquals(tc, 0, n);
    CuAssertPtrNotNull(tc, parts);
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
    /* 高位字节按无符号累加,否则有/无符号 char ABI 上同一输入算出不同值 */
    CuAssertTrue(tc, 255 == hash("\xff", 1));
    CuAssertTrue(tc, 128 == hash("\x80", 1));

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
    CuAssertIntEquals(tc, ERR_OK, sock_pair(fds, 1));
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
    CuAssertIntEquals(tc, ERR_OK, sock_pair(fds, 1));

    // setter：sock_pair 内部已设过 nodelay/nonblock，但显式调用必须返回 ERR_OK
    CuAssertIntEquals(tc, ERR_OK, sock_nodelay(fds[0]));
    CuAssertIntEquals(tc, ERR_OK, sock_nonblock(fds[0]));
    // reuseaddr/reuseport/keepalive/linger 在已连接 fd 上仍可设置（影响新连接或关闭语义）
    CuAssertIntEquals(tc, ERR_OK, sock_reuseaddr(fds[0], 0));
    CuAssertIntEquals(tc, ERR_OK, sock_keepalive(fds[0], 60, 10));
    CuAssertIntEquals(tc, ERR_OK, sock_linger(fds[0]));
    // 读回 keepalive 三项确认真落到内核：断言值与系统默认(idle 7200/intvl 75/cnt 8~9)不同,
    // 故能区分"已设置"与"整块被 #ifdef 跳过"(Windows 走 WSAIoctl,无法逐项读回)
#if !defined(OS_WIN)
    int32_t kaopt;
    socklen_t kalen;
#ifdef TCP_KEEPIDLE
    kaopt = 0;
    kalen = (socklen_t)sizeof(kaopt);
    CuAssertTrue(tc, getsockopt(fds[0], IPPROTO_TCP, TCP_KEEPIDLE, (char *)&kaopt, &kalen) >= 0);
    CuAssertIntEquals(tc, 60, kaopt);
#elif defined(TCP_KEEPALIVE) && !defined(OS_SUN)
    kaopt = 0;
    kalen = (socklen_t)sizeof(kaopt);
    CuAssertTrue(tc, getsockopt(fds[0], IPPROTO_TCP, TCP_KEEPALIVE, (char *)&kaopt, &kalen) >= 0);
    CuAssertIntEquals(tc, 60, kaopt);
#endif
#ifdef TCP_KEEPINTVL
    kaopt = 0;
    kalen = (socklen_t)sizeof(kaopt);
    CuAssertTrue(tc, getsockopt(fds[0], IPPROTO_TCP, TCP_KEEPINTVL, (char *)&kaopt, &kalen) >= 0);
    CuAssertIntEquals(tc, 10, kaopt);
#endif
#ifdef TCP_KEEPCNT
    kaopt = 0;
    kalen = (socklen_t)sizeof(kaopt);
    CuAssertTrue(tc, getsockopt(fds[0], IPPROTO_TCP, TCP_KEEPCNT, (char *)&kaopt, &kalen) >= 0);
    CuAssertIntEquals(tc, 3, kaopt);
#endif
#endif
#if !defined(OS_WIN)
    // SO_REUSEPORT 在 Windows 不支持，跳过
    CuAssertIntEquals(tc, ERR_OK, sock_reuseport(fds[0]));
#endif
    // istcp=1 在 Windows 走 SO_EXCLUSIVEADDRUSE，该选项须在 bind 前设置，故另建裸 socket
    SOCKET lsn = sock_create_cloexec(AF_INET, SOCK_STREAM, 0);
    CuAssertTrue(tc, INVALID_SOCK != lsn);
    CuAssertIntEquals(tc, ERR_OK, sock_reuseaddr(lsn, 1));
    CLOSE_SOCK(lsn);

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

    // 空文件：readall 返 NULL,且自己补上 errno——这条不来自失败的系统调用,
    // 不补的话调用方的 strerror(errno) 打的是上一次调用的陈旧值
    fp = fopen(tmpfile, "wb");
    CuAssertPtrNotNull(tc, fp);
    fclose(fp);
    errno = 0;
    got = 0;
    CuAssertTrue(tc, NULL == readall(tmpfile, &got));
    CuAssertTrue(tc, 0 != errno);

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

/* popen_free 必须自己兜底收尾：它一执行调用方就永久失去 pid，漏调 popen_close 即永久孤儿。
 * 子进程自成进程组后终端信号也够不到它，这条与有没有终端无关。
 * 有了这层兜底，Lua 侧 __gc 才能只留 popen_free 一句 */
static void test_popen_free_reaps(CuTest *tc) {
    popen_ctx ctx;
    char cmd[64];
#ifdef OS_WIN
    SNPRINTF(cmd, sizeof(cmd), "cmd /c \"timeout /t 30 /nobreak\"");
#else
    SNPRINTF(cmd, sizeof(cmd), "sh -c 'sleep 30'");
#endif
    CuAssertIntEquals(tc, ERR_OK, popen_startup(&ctx, cmd, NULL));
#ifndef OS_WIN
    CuAssertTrue(tc, 0 != ctx.pid);
    CuAssertIntEquals(tc, 0, ctx.exited);
#endif
    /* 故意跳过 popen_close，直接 free */
    uint64_t t0 = nowms();
    popen_free(&ctx);
    CuAssertTrue(tc, nowms() - t0 < 5000);/* 同步 SIGKILL + waitpid，不该等满 30 秒 */
#ifndef OS_WIN
    CuAssertIntEquals(tc, 1, ctx.exited);/* free 内部已收尸，没有留下孤儿 */
#endif
    popen_free(&ctx);/* 再 free 一次仍须是 no-op */
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
 * hash_ring 边界场景：空环查找、NULL 入参、重复添加、replicas=0、移除不存在。
 * 末尾三组验 hash_ring_free 的完整复位：原实现只重置链表与 items 指针，
 * nitems/nnodes 留旧值，于是 find 会越过 0 == nitems 早退再解引用已置空的 items、
 * 二次 free 在 FREE(items[i]) 上崩、free 后重用则按旧计数分配却只填尾部，
 * qsort 读到未初始化指针
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

    /* nreplicas 上限：超限拒绝且不留残节点；上限本身可用。
       32 位下 sizeof(指针) * 元素数 会回绕（4 * 2^30 恰为 0，_realloc(ptr,0) 释放并返回 NULL，
       循环随即往 NULL 上写）；64 位不回绕但 8 * 4294967295 ≈ 34GB 分配失败会 exit 掉整个进程。
       两条都能从 Lua 的 ring:add(nreplicas, name) 直接够到——-1 转 uint32 就是 4294967295 */
    CuAssertIntEquals(tc, ERR_FAILED, hash_ring_add(&ring, "huge", 4, 65537u));
    CuAssertIntEquals(tc, ERR_FAILED, hash_ring_add(&ring, "huge", 4, 1073741824u));
    CuAssertIntEquals(tc, ERR_FAILED, hash_ring_add(&ring, "huge", 4, UINT32_MAX));
    CuAssertTrue(tc, 0 == ring.nnodes);
    CuAssertTrue(tc, 0 == ring.nitems);
    CuAssertTrue(tc, NULL == ring.items);
    CuAssertIntEquals(tc, ERR_OK, hash_ring_add(&ring, "atcap", 5, 65536u));
    CuAssertTrue(tc, 65536u == ring.nitems);
    hash_ring_free(&ring);

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
    CuAssertTrue(tc, 0 == ring.nnodes);
    CuAssertTrue(tc, 0 == ring.nitems);
    CuAssertTrue(tc, NULL == hash_ring_find(&ring, "any", 3));

    hash_ring_free(&ring);

    CuAssertIntEquals(tc, ERR_OK, hash_ring_add(&ring, "reuse", 5, 10));
    CuAssertTrue(tc, 1 == ring.nnodes);
    CuAssertTrue(tc, 10 == ring.nitems);
    CuAssertTrue(tc, NULL != hash_ring_find(&ring, "any", 3));
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
 * %m 已给出时负 tm_yday 同样要修正：修正块原先嵌在 !HAVE_MON 分支内，月份已知就跳过，
 * 于是 tm_mday 拿负 yday 直接算出负数（"2024 05 00" 得 -121），mktime 归一成
 * 一个错得离谱却"成功"的时间戳，调用方拿不到任何失败信号
 * ======================================================================= */
static void test_strptime_neg_yday_with_mon(CuTest *tc) {
    struct tm tm;
    char *end;

    ZERO(&tm, sizeof(tm));
    end = _strptime("2024 05 00", "%Y %m %U", &tm);
    CuAssertPtrNotNull(tc, end);
    CuAssertTrue(tc, '\0' == *end);
    /* %m 与 %U 本就自相矛盾，修正只保证：年份回滚到上一年、yday/mday 不再是负数
     * （改前 year=124 yday=-1 mday=-121）。mday 按给定月份 + 回滚后的 yday 推出，
     * 可以超过月长（此例 245），交给 mktime 归一，不在此断言月内范围 */
    CuAssertIntEquals(tc, 123, tm.tm_year);
    CuAssertTrue(tc, tm.tm_yday >= 0);
    CuAssertTrue(tc, tm.tm_mday >= 1);

    /* %W 同路径 */
    ZERO(&tm, sizeof(tm));
    end = _strptime("2024 03 00", "%Y %m %W", &tm);
    CuAssertPtrNotNull(tc, end);
    CuAssertIntEquals(tc, 123, tm.tm_year);
    CuAssertTrue(tc, tm.tm_yday >= 0);
    CuAssertTrue(tc, tm.tm_mday >= 1);

    /* 月份已知 + 正 yday 的常规路径不受影响 */
    ZERO(&tm, sizeof(tm));
    end = _strptime("2024 03 10", "%Y %m %U", &tm);
    CuAssertPtrNotNull(tc, end);
    CuAssertTrue(tc, tm.tm_mday >= 1 && tm.tm_mday <= 31);
    CuAssertTrue(tc, 124 == tm.tm_year);
}

/* sectostr 忽略 LOCALTIME 失败的话，未初始化的 struct tm 会交给 strftime，
 * fmt 含 %a/%b 时 libc 拿不定的 tm_wday/tm_mon 去索引静态名字表即越界读。
 * 取 INT64_MAX：年份远超 tm_year 的 int 量程，localtime 必败（实测失败阈值约 6.7768e16 秒）。
 * 不能取 UINT64_MAX——转 64 位 time_t 是 -1 即 1969 年，是合法时刻。
 * 32 位 time_t 会把大值截成任意合法秒数，构造不出失败，故整段跳过。
 * mstostr 的 LOCALTIME 分支不在此断言：它先 /1000，uint64 入参最大只到 1.84e16 秒，
 * 够不到上面那个阈值，64 位 time_t 下不可达（守卫仍留着，32 位截断时是活的）。
 * 它另有一条失败分支：秒级串占满以后 " 000" 后缀装不下，见本用例末尾 */
static void test_timestr_out_of_range(CuTest *tc) {
    char buf[TIME_LENS];

    if (sizeof(time_t) >= 8) {
        memset(buf, 'x', sizeof(buf));
        CuAssertIntEquals(tc, ERR_FAILED, sectostr((uint64_t)INT64_MAX, "%Y-%m-%d %a %b", buf));
        CuAssertTrue(tc, '\0' == buf[0]);
    }

    /* 正常时刻仍照常工作 */
    CuAssertIntEquals(tc, ERR_OK, sectostr(nowsec(), "%Y-%m-%d", buf));
    CuAssertTrue(tc, '\0' != buf[0]);
    CuAssertIntEquals(tc, ERR_OK, mstostr(nowms(), "%Y-%m-%d %H:%M:%S", buf));
    CuAssertTrue(tc, '\0' != buf[0]);

    /* fmt 长到 124 字节：sectostr 装得下(strftime 上限 TIME_LENS-1)，
     * mstostr 还要 " 000" 那 5 个字节就装不下 —— 不截断，整体失败 */
    char longfmt[125];
    memset(longfmt, 'x', sizeof(longfmt) - 1);
    longfmt[sizeof(longfmt) - 1] = '\0';
    CuAssertIntEquals(tc, ERR_OK, sectostr(nowsec(), longfmt, buf));
    CuAssertIntEquals(tc, 124, (int)strlen(buf));
    CuAssertIntEquals(tc, ERR_FAILED, mstostr(nowms(), longfmt, buf));
    CuAssertTrue(tc, '\0' == buf[0]);
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
 * 时间轮自适应睡眠：睡到下一个真正到期的 jiffy，而非固定 1ms tick。
 * 上面的 test_tw / test_tw_long_timeout 只断言"最终触发了"，一个迟到 250ms 的
 * 回归照样全绿，所以这里两条分别守：
 *   1) 各档超时按时触发（tv1 近端/远端、cascade 边界两侧、tv2 降级回捞）
 *   2) 轮线程长睡期间新加的定时器能被 tw_add 的 CAS+signal 及时唤起（丢信号窗口）
 * 没有守住的：空闲时的唤醒频率。原来靠 tw_ctx.nloop 计数，但那个字段是轮线程写、
 * 测试线程读的普通整数，tsan 会报 race，故连字段带用例一并删掉。退回固定 1ms tick
 * 的回归目前无人拦得住 —— 要补的话得让 _tw_next_delta 可单独测（单线程喂一个只有
 * 远档的轮子，断言算出的 sleep_ms 远大于 1），而不是再引一个跨线程计数器
 * ======================================================================= */
// 8u 而非 8：下面多处与 size_t / uint64_t 比较，无符号常量免掉 -Wsign-compare
#define TW_LAT_N 8u
// 255/256 是 tv1 与 tv2 的分界（idx < TVR_SIZE 才留 tv1），两侧各取一档
static const uint32_t _tw_lat_ms[TW_LAT_N] = { 1, 5, 50, 200, 255, 256, 300, 600 };
static atomic64_t _tw_lat_fire[TW_LAT_N];// 各档实际触发时刻(绝对毫秒)，0 表示未触发
static timer_ctx _tw_lat_timer;
// 用 ud->sess 携带档位序号，回调里记下触发时刻
static void _tw_lat_cb(ud_cxt *ud) {
    if (ud->sess < TW_LAT_N) {
        ATOMIC64_SET(&_tw_lat_fire[ud->sess], (atomic64_t)timer_cur_ms(&_tw_lat_timer));
    }
}
static void test_tw_latency(CuTest *tc) {
    tw_ctx tw;
    ud_cxt ud;
    uint64_t start;
    int64_t fire, expect, late;
    int32_t waited;
    size_t i;

    // timer_cur_ms 取的是单调绝对毫秒，与时间轮内部那份同刻度
    timer_init(&_tw_lat_timer);
    for (i = 0; i < TW_LAT_N; i++) {
        ATOMIC64_SET(&_tw_lat_fire[i], 0);
    }
    tw_init(&tw, 0, NULL);
    start = timer_cur_ms(&_tw_lat_timer);
    for (i = 0; i < TW_LAT_N; i++) {
        ZERO(&ud, sizeof(ud));
        ud.sess = (uint64_t)i;
        tw_add(&tw, _tw_lat_ms[i], _tw_lat_cb, NULL, &ud);
    }
    // 最长一档 600ms，等到它触发即说明全部触发
    waited = 0;
    while (waited < 2000 && 0 == ATOMIC64_GET(&_tw_lat_fire[TW_LAT_N - 1])) {
        MSLEEP(20);
        waited += 20;
    }
    tw_free(&tw);

    for (i = 0; i < TW_LAT_N; i++) {
        fire = (int64_t)ATOMIC64_GET(&_tw_lat_fire[i]);
        CuAssertTrue(tc, 0 != fire);
        expect = (int64_t)start + (int64_t)_tw_lat_ms[i];
        late = fire - expect;
        // 不可能早于到期(expires 由 tw_add 按 start 之后的时刻算出)
        CuAssertTrue(tc, late >= 0);
        // 迟到容忍 100ms 扛 CI 调度抖动；睡过头的回归是 250ms 量级，拦得住
        CuAssertTrue(tc, late < 100);
    }
}
static void test_tw_wakeup_after_idle(CuTest *tc) {
    tw_ctx tw;
    ud_cxt ud;
    int32_t waited;

    tw_init(&tw, 0, NULL);
    ZERO(&ud, sizeof(ud));
    // 先挂一个很远的定时器：tv1 长期为空，轮线程只在 cascade 边界醒，即处于长睡
    tw_add(&tw, 5000, _tw_cb, NULL, &ud);
    MSLEEP(300);
    // 长睡中投一个近档：只能靠 tw_add 的 CAS+signal 唤起。若丢信号窗口没封，
    // signal 会打空，这一档最坏要拖到下一个 cascade 边界(≤256ms)才被拾起
    ATOMIC_SET(&_tw_fired, 0);
    tw_add(&tw, 2, _tw_cb, NULL, &ud);
    waited = 0;
    while (waited < 500 && ATOMIC_GET(&_tw_fired) < 1) {
        MSLEEP(5);
        waited += 5;
    }
    tw_free(&tw);
    CuAssertIntEquals(tc, 1, ATOMIC_GET(&_tw_fired));
    CuAssertTrue(tc, waited < 100);
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
static pool_cbs _pt_cbs = { _pt_elnew, _pt_elfree, _pt_elreset, _pt_elclear };

// 取/还/复用:空池 pop 走 _elnew,push 走 _elclear,命中 pop 走 _elreset(不再 new),args 透传
static void _pool_basic_check(CuTest *tc, int32_t thsafe) {
    pool_ctx pool;
    pool_t_obj *o, *o2;
    int32_t arg = 7;
    _pt_counters_reset();
    pool_init(&pool, sizeof(pool_t_obj), 8, 2, thsafe, &_pt_cbs);
    CuAssertIntEquals(tc, 0, pool_size(&pool));
    CuAssertTrue(tc, pool_capacity(&pool) >= 8);
    o = (pool_t_obj *)pool_pop(&pool, &arg, 0);
    CuAssertPtrNotNull(tc, o);
    CuAssertTrue(tc, POOL_T_MAGIC == o->magic);
    CuAssertIntEquals(tc, 1, _pt_new);
    CuAssertPtrEquals(tc, &arg, _pt_new_args);
    CuAssertIntEquals(tc, 0, pool_size(&pool));
    pool_push(&pool, o, 0);
    CuAssertIntEquals(tc, 1, _pt_clear);
    CuAssertIntEquals(tc, 1, pool_size(&pool));
    o2 = (pool_t_obj *)pool_pop(&pool, &arg, 0);
    CuAssertPtrEquals(tc, o, o2); // 命中复用,同一对象
    CuAssertIntEquals(tc, 1, _pt_new); // 未新建
    CuAssertIntEquals(tc, 1, _pt_reset);
    CuAssertPtrEquals(tc, &arg, _pt_reset_args);
    CuAssertIntEquals(tc, 0, pool_size(&pool));
    pool_push(&pool, o2, 0);
    pool_free(&pool);
    CuAssertIntEquals(tc, 1, _pt_free);
    CuAssertIntEquals(tc, 0, _pt_free_bad);
}
static void test_pool_basic(CuTest *tc) {
    _pool_basic_check(tc, 0);
    _pool_basic_check(tc, 1);
}
// 满池:pool_push 满则 _elfree;POOL_OP_NOFREE 满则不释放,对象仍归调用方
static void test_pool_full(CuTest *tc) {
    pool_ctx pool;
    pool_t_obj *objs[4], *over;
    uint32_t i;
    _pt_counters_reset();
    pool_init(&pool, sizeof(pool_t_obj), 4, 0, 0, &_pt_cbs);
    CuAssertIntEquals(tc, 4, pool_capacity(&pool));
    for (i = 0; i < 4; i++) {
        objs[i] = (pool_t_obj *)pool_pop(&pool, NULL, 0);
    }
    for (i = 0; i < 4; i++) {
        pool_push(&pool, objs[i], 0);
    }
    CuAssertIntEquals(tc, 4, pool_size(&pool));
    CuAssertIntEquals(tc, 0, _pt_free);
    // 满池 pool_push:先 clear 再尝试入池;池满则 _elfree
    over = (pool_t_obj *)_pt_elnew(NULL);
    pool_push(&pool, over, 0);
    CuAssertIntEquals(tc, 1, _pt_free);
    CuAssertIntEquals(tc, 4, pool_size(&pool));
    pool_free(&pool);
    CuAssertIntEquals(tc, 5, _pt_free); // 1(push 满)+4(pool_free)
    CuAssertIntEquals(tc, 0, _pt_free_bad);
    // POOL_OP_NOFREE: 满时 pool_push 不释放,对象仍归调用方
    _pt_counters_reset();
    pool_init(&pool, sizeof(pool_t_obj), 4, 0, 0, &_pt_cbs);
    for (i = 0; i < 4; i++) {
        objs[i] = (pool_t_obj *)pool_pop(&pool, NULL, 0);
    }
    for (i = 0; i < 4; i++) {
        pool_push(&pool, objs[i], 0);
    }
    over = (pool_t_obj *)_pt_elnew(NULL);
    int32_t rt = pool_push(&pool, over, POOL_OP_NOFREE); // 满+NOFREE:不释放
    CuAssertIntEquals(tc, ERR_FAILED, rt);
    CuAssertIntEquals(tc, 0, _pt_free);
    CuAssertIntEquals(tc, 4, pool_size(&pool));
    FREE(over); // 调用方手动释放
    pool_free(&pool);
    CuAssertIntEquals(tc, 4, _pt_free); // 0(push 满)+4(pool_free)
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
        objs[i] = (pool_t_obj *)pool_pop(&pool, NULL, 0);
    }
    for (i = 0; i < 8; i++) {
        pool_push(&pool, objs[i], 0);
    }
    CuAssertIntEquals(tc, 8, pool_size(&pool));
    pool_shrink_to(&pool, 3); // keep=max(3,nkeep=2)=3,释放 5
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
        objs[i] = (pool_t_obj *)pool_pop(&pool, NULL, 0);
    }
    for (i = 0; i < 8; i++) {
        pool_push(&pool, objs[i], 0);
    }
    pool_shrink_to(&pool, 0); // 首次 prev=0 不忙;keep=max(0,2)=2
    CuAssertIntEquals(tc, 2, pool_size(&pool));
    pool_free(&pool);
    // busy 跳过:采样骤降(8→2,跌幅 >20%)后,下一次收缩被跳过
    _pt_counters_reset();
    pool_init(&pool, sizeof(pool_t_obj), 16, 0, 0, &_pt_cbs);
    for (i = 0; i < 8; i++) {
        objs[i] = (pool_t_obj *)pool_pop(&pool, NULL, 0);
    }
    for (i = 0; i < 8; i++) {
        pool_push(&pool, objs[i], 0);
    }
    pool_shrink_to(&pool, 2); // 首次:不忙,8→2,记录 prev=8
    CuAssertIntEquals(tc, 2, pool_size(&pool));
    pool_shrink_to(&pool, 0); // cur=2 < prev(8)*4/5 → 忙 → 跳过
    CuAssertIntEquals(tc, 2, pool_size(&pool));
    pool_free(&pool);
}
// 默认回调(NULL):走 CALLOC/FREE;容量 0 用默认值
static void test_pool_default(CuTest *tc) {
    pool_ctx pool;
    uint64_t *a, *b;
    pool_init(&pool, sizeof(uint64_t), 0, 4, 0, NULL);
    CuAssertTrue(tc, pool_capacity(&pool) >= 1024); // POOL_DEFAULT_CAP
    a = (uint64_t *)pool_pop(&pool, NULL, 0);
    CuAssertPtrNotNull(tc, a);
    CuAssertTrue(tc, 0 == *a); // CALLOC 清零
    *a = 12345;
    pool_push(&pool, a, 0);
    b = (uint64_t *)pool_pop(&pool, NULL, 0);
    CuAssertPtrEquals(tc, a, b); // 命中复用
    pool_push(&pool, b, 0);
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
// strtod_c：按 C locale 解析 '.' 浮点，不受进程 LC_NUMERIC 影响；先测基础正确性，
// 逗号小数点 locale 可用时再对照验证（不可用则仅跑基础用例，保证可移植）
static void test_strtod_c(CuTest *tc) {
    char *end;
    double base = strtod_c("3.14", &end);
    char baseend = *end;
    // 保存当前 LC_NUMERIC（setlocale 返回的静态串可能被后续调用覆写，须复制）
    char savedbuf[64] = { 0 };
    const char *saved = setlocale(LC_NUMERIC, NULL);
    if (NULL != saved) {
        SNPRINTF(savedbuf, sizeof(savedbuf), "%s", saved);
    }
    // 尝试切到逗号小数点 locale（覆盖 POSIX 与 Windows 常见命名）
    const char *candidates[] = {
        "de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "nl_NL.UTF-8",
        "German_Germany.1252", "de-DE"
    };
    int32_t comma_ok = 0;
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (NULL != setlocale(LC_NUMERIC, candidates[i])
            && ',' == localeconv()->decimal_point[0]) {
            comma_ok = 1;
            break;
        }
    }
    double cv = 0.0, sv = 0.0;
    char cvend = 0, svend = 0;
    if (comma_ok) {
        cv = strtod_c("3.14", &end);
        cvend = *end;
        sv = strtod("3.14", &end);
        svend = *end;
    }
    // 先还原 locale 再断言：CuAssert 失败会 longjmp，若在还原前断言会污染后续测试
    setlocale(LC_NUMERIC, ('\0' != savedbuf[0]) ? savedbuf : "C");
    // 基础正确性（任意 locale）
    CuAssertDblEquals(tc, 3.14, base, 1e-9);
    CuAssertTrue(tc, '\0' == baseend);
    // 逗号 locale 可用时：strtod_c 仍解析 3.14，普通 strtod 截断为 3.0 停在 '.'
    if (comma_ok) {
        CuAssertDblEquals(tc, 3.14, cv, 1e-9);
        CuAssertTrue(tc, '\0' == cvend);
        CuAssertDblEquals(tc, 3.0, sv, 1e-9);
        CuAssertTrue(tc, '.' == svend);
    }
}

// str2u64：按长度解析十进制无符号整数。收敛了原先散在 scram / http / mysql / coro_utils /
// debug_console / harbor 六处的 strtoXX 用法，故这里把它们各自依赖的边界一次测全
static void test_str2u64(CuTest *tc) {
    uint64_t v;

    // 1) 基本正确性 + 上界恰好命中
    v = 0;
    CuAssertIntEquals(tc, ERR_OK, str2u64("0", 1, UINT64_MAX, &v));
    CuAssertTrue(tc, 0 == v);
    CuAssertIntEquals(tc, ERR_OK, str2u64("65535", 5, UINT16_MAX, &v));
    CuAssertTrue(tc, UINT16_MAX == v);
    CuAssertIntEquals(tc, ERR_FAILED, str2u64("65536", 5, UINT16_MAX, &v));

    // 2) 只吃 lens 个字节，尾部残留不参与——harbor 的 url_decode 场景
    // ("%310" 解码成 "10" 只缩短 lens，缓冲里仍读得到 "1010")
    CuAssertIntEquals(tc, ERR_OK, str2u64("1010", 2, UINT64_MAX, &v));
    CuAssertTrue(tc, 10 == v);

    // 3) strtoXX 会静默收下、这里必须拒的几种：前导空白 / 正负号 / 尾随垃圾 / 内嵌非数字
    CuAssertIntEquals(tc, ERR_FAILED, str2u64(" 5", 2, UINT64_MAX, &v));
    CuAssertIntEquals(tc, ERR_FAILED, str2u64("+5", 2, UINT64_MAX, &v));
    CuAssertIntEquals(tc, ERR_FAILED, str2u64("-1", 2, UINT64_MAX, &v));
    CuAssertIntEquals(tc, ERR_FAILED, str2u64("1x", 2, UINT64_MAX, &v));
    CuAssertIntEquals(tc, ERR_FAILED, str2u64("1 2", 3, UINT64_MAX, &v));
    CuAssertIntEquals(tc, ERR_FAILED, str2u64("abc", 3, UINT64_MAX, &v));

    // 4) 空输入
    CuAssertIntEquals(tc, ERR_FAILED, str2u64("", 0, UINT64_MAX, &v));
    CuAssertIntEquals(tc, ERR_FAILED, str2u64(NULL, 0, UINT64_MAX, &v));
    CuAssertIntEquals(tc, ERR_FAILED, str2u64(NULL, 3, UINT64_MAX, &v));

    // 5) uint64 边界：恰好 UINT64_MAX 收下，末位再 +1 与多一位都要拒
    CuAssertIntEquals(tc, ERR_OK, str2u64("18446744073709551615", 20, UINT64_MAX, &v));
    CuAssertTrue(tc, UINT64_MAX == v);
    CuAssertIntEquals(tc, ERR_FAILED, str2u64("18446744073709551616", 20, UINT64_MAX, &v));
    CuAssertIntEquals(tc, ERR_FAILED, str2u64("99999999999999999999", 20, UINT64_MAX, &v));

    // 6) 前导零不影响判定，也不该被当成溢出
    CuAssertIntEquals(tc, ERR_OK, str2u64("000000000000000000000042", 24, UINT64_MAX, &v));
    CuAssertTrue(tc, 42 == v);

    // 7) 失败时不得写 out —— debug_console 的 loglv 依赖这一点(失败即整条拒绝)
    v = 0x5a5a5a5a;
    CuAssertIntEquals(tc, ERR_FAILED, str2u64("9", 1, 4, &v));
    CuAssertTrue(tc, 0x5a5a5a5a == v);
}

void test_utils(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_pack_unpack);
    SUITE_ADD_TEST(suite, test_binary);
    SUITE_ADD_TEST(suite, test_binary_extra);
    SUITE_ADD_TEST(suite, test_binary_set_binary_self_alias);
    SUITE_ADD_TEST(suite, test_binary_bounds);
    SUITE_ADD_TEST(suite, test_buffer);
    SUITE_ADD_TEST(suite, test_buffer_search_start_overflow);
    SUITE_ADD_TEST(suite, test_sfid_clockback_giveup);
    SUITE_ADD_TEST(suite, test_sfid_epoch_underflow);
    SUITE_ADD_TEST(suite, test_buffer_extra);
    SUITE_ADD_TEST(suite, test_buffer_external_appendv);
    SUITE_ADD_TEST(suite, test_buffer_external_not_writable);
    SUITE_ADD_TEST(suite, test_buffer_external_zero);
    SUITE_ADD_TEST(suite, test_buffer_get_commit);
    SUITE_ADD_TEST(suite, test_buffer_space);
    SUITE_ADD_TEST(suite, test_buffer_from_sock_space);
    SUITE_ADD_TEST(suite, test_buffer_free_resets);
    SUITE_ADD_TEST(suite, test_buffer_hint_after_migrate);
    SUITE_ADD_TEST(suite, test_sfid);
    SUITE_ADD_TEST(suite, test_sfid_invalid);
    SUITE_ADD_TEST(suite, test_hash_ring);
    SUITE_ADD_TEST(suite, test_hash_ring_edge);
    SUITE_ADD_TEST(suite, test_netaddr);
    SUITE_ADD_TEST(suite, test_netaddr_extra);
    SUITE_ADD_TEST(suite, test_chan);
    SUITE_ADD_TEST(suite, test_hug);
    SUITE_ADD_TEST(suite, test_hug_wakeup_flood);
    SUITE_ADD_TEST(suite, test_timeofday_consistent);
    SUITE_ADD_TEST(suite, test_timer);
    SUITE_ADD_TEST(suite, test_timer_extra);
    SUITE_ADD_TEST(suite, test_load_trend);
    SUITE_ADD_TEST(suite, test_utils_misc);
    SUITE_ADD_TEST(suite, test_popen2);
    SUITE_ADD_TEST(suite, test_popen_close);
    SUITE_ADD_TEST(suite, test_popen_free_reaps);
    SUITE_ADD_TEST(suite, test_log_lv);
    SUITE_ADD_TEST(suite, test_log_slog_filter);
    SUITE_ADD_TEST(suite, test_strptime);
    SUITE_ADD_TEST(suite, test_strptime_invalid);
    SUITE_ADD_TEST(suite, test_strptime_week_rollover);
    SUITE_ADD_TEST(suite, test_strptime_week_neg_yday);
    SUITE_ADD_TEST(suite, test_strptime_neg_yday_with_mon);
    SUITE_ADD_TEST(suite, test_timestr_out_of_range);
    SUITE_ADD_TEST(suite, test_tw);
    SUITE_ADD_TEST(suite, test_tw_long_timeout);
    SUITE_ADD_TEST(suite, test_tw_latency);
    SUITE_ADD_TEST(suite, test_tw_wakeup_after_idle);
    SUITE_ADD_TEST(suite, test_mem_helpers);
    SUITE_ADD_TEST(suite, test_safe_fill_str);
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
    SUITE_ADD_TEST(suite, test_strtod_c);
    SUITE_ADD_TEST(suite, test_str2u64);
}
