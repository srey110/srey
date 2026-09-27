#include "test_utils.h"
#include "lib.h"
#include "utils/strptime.h"
#include "utils/pool.h"
#include "utils/uuid.h"
#include <locale.h>

#define FAKE_RV_MAX 3// 场景二是最长的一路: 两轮读满 + 一轮确认
// 8u 而非 8：下面多处与 size_t / uint64_t 比较，无符号常量免掉 -Wsign-compare
#define TW_LAT_N 8u
// 带标记的测试对象:_elfree 收到真实对象时 magic 必为 POOL_T_MAGIC;
// 若收到队列槽位地址(历史 bug),magic 不符,_pt_free_bad 增长
#define POOL_T_MAGIC 0x5ada5adau
#define UUID_MT_THREADS 4// uuid 多线程用例的线程数
#define UUID_MT_PER 20000// 每个线程生成的 v7 个数
#define MEMSTR_HAY 48// memstr 随机比对用例的最大源长度
#define MEMSTR_ROUNDS 6000// memstr 随机比对每种 ncs 的轮数
#define ID_MT_THREADS 4// createid 多线程用例的线程数
#define ID_MT_PER 5000// 每个线程发的号数，超过一段(1024)才能跨段

// createid 多线程用例：每个线程把发到的号顺序存进自己那段 ids
typedef struct _id_mt_arg {
    uint64_t *ids;
} _id_mt_arg;
// uuid 多线程用例：每个线程把生成的 v7 顺序存进自己那段 ids
typedef struct _uuid_mt_arg {
    int32_t n;
    int32_t nfail;
    char *ids;
} _uuid_mt_arg;

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

    /* 网络字节序宏（64 位）：往返自洽对恒等函数也成立，钉字节序得比对大端布局字面量 */
    uint64_t v64 = 0x0102030405060708ULL;
    uint64_t net = htonll(v64);
    const uint8_t bigend[8] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
    CuAssertTrue(tc, 0 == memcmp(&net, bigend, sizeof(bigend)));
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
    binary_init_write(&bw, 0, 64); /* 动态分配，初始不设缓冲 */

    /* 写入各种类型 */
    int8_t i8 = -120;
    binary_set_int8(&bw, i8);
    uint8_t u8 = 200;
    binary_set_uint8(&bw, u8);
    int16_t i16 = -30000;
    binary_set_integer(&bw, i16, 2, 1);
    uint16_t u16 = 60000;
    binary_set_uinteger(&bw, u16, 2, 1);
    int32_t i32 = -1000000;
    binary_set_integer(&bw, i32, 4, 1);
    uint32_t u32 = 3000000;
    binary_set_uinteger(&bw, u32, 4, 0); /* 大端 */
    int64_t i64 = -9876543210LL;
    binary_set_integer(&bw, i64, 8, 1);
    float   fv = -3.14159f; binary_set_float(&bw, fv, 1);
    double  dv = 2.718281828; binary_set_double(&bw, dv, 1);
    binary_set_fill(&bw, 0xAB, 4);/* 填充 4 字节 0xAB */
    binary_set_skip(&bw, 2);/* 只推进 offset，不写内容（binary.h: expand 后 offset += lens）*/
    const char *str = "hello";
    binary_set_string(&bw, str);/* 含 \0 */
    const char *bin = "world";
    binary_set_binary(&bw, bin, 5);/* 不含 \0 */

    /* 读取并逐一验证 */
    binary_ctx br;
    binary_init_read(&br, bw.data, bw.offset);

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

    /* 填充字节要读回来比对：只 skip 过去的话，binary_set_fill 写错值甚至什么都不写都发现不了 */
    const uint8_t *fill = (const uint8_t *)binary_get_binary(&br, 4);
    CuAssertPtrNotNull(tc, fill);
    CuAssertTrue(tc, 0xAB == fill[0] && 0xAB == fill[1] && 0xAB == fill[2] && 0xAB == fill[3]);
    /* set_skip 那 2 字节内容未定义，只跳过 */
    binary_get_skip(&br, 2);

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
    binary_init_write(&bin, 0, 0);
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
    binary_init_write(&bs, 0, 0);
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
    binary_init_write(&bov, 0, 0);
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
    binary_init_read(&br, raw, sizeof(raw));

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
    binary_init_read(&bn, nul, sizeof(nul));
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

    /* 连续生成的 ID 单调递增，且每个都 decode 回来核对三个字段 */
    uint64_t ts = 0, pts, cur;
    int32_t mid, seq, pseq;
    uint64_t before = nowms();
    uint64_t prev = sfid_id(&ctx);
    sfid_decode(&ctx, prev, &pts, &mid, &pseq);
    CuAssertIntEquals(tc, 1, mid);
    for (int i = 0; i < 100; i++) {
        cur = sfid_id(&ctx);
        CuAssertTrue(tc, cur > prev);
        sfid_decode(&ctx, cur, &ts, &mid, &seq);
        CuAssertIntEquals(tc, 1, mid);
        CuAssertTrue(tc, seq >= 0 && seq <= ctx.sequencemask);
        /* 同毫秒内序号加一；跨毫秒则时间戳前进、序号归零 */
        if (ts == pts) {
            CuAssertIntEquals(tc, pseq + 1, seq);
        } else {
            CuAssertTrue(tc, ts > pts && 0 == seq);
        }
        prev = cur;
        pts = ts;
        pseq = seq;
    }

    /* decode 出的时间戳是含 customepoch 的墙钟毫秒，必须夹在生成前后取的 nowms 之间。
       原来的 ts > 0 恒真：customepoch 默认非 0，加上它之后与 ID 内容无关。
       两端各留 2s 余量，只挡量纲与纪元错位，不挡调度抖动 */
    uint64_t after = nowms();
    CuAssertTrue(tc, ts + 2000 >= before && ts <= after + 2000);
}

/* 同毫秒序号耗尽：sequencemask 只有 1 位，第 3 次调用即撞上回绕保护，自旋等下一毫秒。
 * 回绕保护若被删掉，sequence 会越过掩码，(sequence & sequencemask) 折回去与前面的 ID 撞号 */
static void test_sfid_seq_exhaust(CuTest *tc) {
    sfid_ctx ctx;
    uint64_t cur;
    uint64_t prev = 0;
    CuAssertPtrNotNull(tc, sfid_init(&ctx, 1, 21, 1, 0));
    CuAssertIntEquals(tc, 1, ctx.sequencemask);
    for (int i = 0; i < 32; i++) {
        cur = sfid_id(&ctx);
        CuAssertTrue(tc, 0 != cur);
        CuAssertTrue(tc, cur > prev);
        prev = cur;
    }
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
 * uuid —— v4 / v7 生成、版本号、文本互转
 * ======================================================================= */
// RFC 9562 附录的两个示例：往返、版本号、v7 时间戳
static void test_uuid_rfc_vectors(CuTest *tc) {
    const char *v7 = "017F22E2-79B0-7CC3-98C4-DC0C0C07398F";// A.6，输入大写
    const char *v4 = "919108f7-52d1-4320-9bac-f847db4148a8";// A.3
    // 逐字节写出期望值，挡住 fromstr / tostr 错得对称
    const unsigned char v7bin[UUID_LENS] = { 0x01, 0x7F, 0x22, 0xE2, 0x79, 0xB0, 0x7C, 0xC3,
                                             0x98, 0xC4, 0xDC, 0x0C, 0x0C, 0x07, 0x39, 0x8F };
    char u[UUID_LENS];
    char s[UUID_STR_LENS];
    CuAssertIntEquals(tc, ERR_OK, uuid_fromstr(v7, strlen(v7), u));
    CuAssertTrue(tc, 0 == memcmp(u, v7bin, UUID_LENS));
    memset(s, 'X', sizeof(s));
    uuid_tostr(u, s);
    CuAssertStrEquals(tc, "017f22e2-79b0-7cc3-98c4-dc0c0c07398f", s);// 输出固定小写
    CuAssertIntEquals(tc, 7, uuid_version(u));
    CuAssertTrue(tc, 0x017F22E279B0ULL == uuid_v7_ms(u));

    CuAssertIntEquals(tc, ERR_OK, uuid_fromstr(v4, strlen(v4), u));
    uuid_tostr(u, s);
    CuAssertStrEquals(tc, v4, s);
    CuAssertIntEquals(tc, 4, uuid_version(u));
    CuAssertTrue(tc, 0 == uuid_v7_ms(u));
}
// 非法输入全返 ERR_FAILED；大小写、Nil / Max、不以 '\0' 结尾的缓冲都能解析
static void test_uuid_fromstr_invalid(CuTest *tc) {
    const char *ok = "017f22e2-79b0-7cc3-98c4-dc0c0c07398f";
    const char *bad[] = {
        "017f22e-279b0-7cc3-98c4-dc0c0c07398f",// 连字符前移一位
        "017f22e279-b0-7cc3-98c4-dc0c0c07398f",// 连字符后移两位
        "017f22e2079b007cc3098c40dc0c0c07398f",// 连字符全换成 0
        "017f22e2--9b0-7cc3-98c4-dc0c0c07398f",// 多一个连字符
        "017f22e2-79b0-7cc3-98c4+dc0c0c07398f",
        "017f22e2-79b0-7cc3-98c4-dc0c0c07398g",
        "017f22e2-79b0-7cc3-98c4-dc0c0c0739 f",
        "{17f22e2-79b0-7cc3-98c4-dc0c0c07398}",// 36 字节的花括号形式
        "urn:uuid:017f22e2-79b0-7cc3-98c4-dc0",// 36 字节的 urn 前缀
    };
    // 十六进制边界两侧的字符 + 连字符 + 一个合法数字
    const char *edge = "/:@G`g-0";
    char u[UUID_LENS];
    char ref[UUID_LENS];
    char s[UUID_STR_LENS];
    char buf[64];
    char *exact;
    size_t i, j;
    int32_t isdash, valid;
    CuAssertIntEquals(tc, ERR_OK, uuid_fromstr(ok, strlen(ok), ref));
    for (i = 0; i < ARRAY_SIZE(bad); i++) {
        CuAssertIntEquals(tc, 36, (int)strlen(bad[i]));// 长度对了才测得到别的检查
        CuAssertIntEquals(tc, ERR_FAILED, uuid_fromstr(bad[i], strlen(bad[i]), u));
    }
    // 36 个位置逐个替换：连字符位只认 '-'，其余位只认十六进制
    for (i = 0; i < 36; i++) {
        isdash = (8 == i || 13 == i || 18 == i || 23 == i);
        for (j = 0; j < strlen(edge); j++) {
            memcpy(buf, ok, 36);
            buf[i] = edge[j];
            valid = isdash ? ('-' == edge[j]) : ('0' == edge[j]);
            CuAssertIntEquals(tc, valid ? ERR_OK : ERR_FAILED, uuid_fromstr(buf, 36, u));
        }
    }
    // 长度必须恰好 36
    CuAssertIntEquals(tc, ERR_FAILED, uuid_fromstr(ok, 35, u));
    memcpy(buf, ok, 36);
    buf[36] = '0';
    CuAssertIntEquals(tc, ERR_FAILED, uuid_fromstr(buf, 37, u));
    CuAssertIntEquals(tc, ERR_FAILED, uuid_fromstr(buf, 0, u));
    CuAssertIntEquals(tc, ERR_FAILED, uuid_fromstr("{017f22e2-79b0-7cc3-98c4-dc0c0c07398f}", 38, u));
    CuAssertIntEquals(tc, ERR_FAILED, uuid_fromstr("urn:uuid:017f22e2-79b0-7cc3-98c4-dc0c0c07398f", 45, u));
    // 大写、混合大小写
    CuAssertIntEquals(tc, ERR_OK, uuid_fromstr("017F22E2-79B0-7CC3-98C4-DC0C0C07398F", 36, u));
    CuAssertTrue(tc, 0 == memcmp(u, ref, UUID_LENS));
    CuAssertIntEquals(tc, ERR_OK, uuid_fromstr("017f22E2-79B0-7cC3-98c4-Dc0C0c07398F", 36, u));
    CuAssertTrue(tc, 0 == memcmp(u, ref, UUID_LENS));
    // 恰好 36 字节、后面没有 '\0' 的堆缓冲：越界读会被 ASan 抓到
    MALLOC(exact, 36);
    memcpy(exact, ok, 36);
    CuAssertIntEquals(tc, ERR_OK, uuid_fromstr(exact, 36, u));
    CuAssertTrue(tc, 0 == memcmp(u, ref, UUID_LENS));
    FREE(exact);
    // 后面紧跟非 '\0' 字符
    memcpy(buf, ok, 36);
    memcpy(buf + 36, "ffff", 5);
    CuAssertIntEquals(tc, ERR_OK, uuid_fromstr(buf, 36, u));
    CuAssertTrue(tc, 0 == memcmp(u, ref, UUID_LENS));
    // Nil、Max 能解析，但变体不是 10，版本号与毫秒都得 0
    CuAssertIntEquals(tc, ERR_OK, uuid_fromstr("00000000-0000-0000-0000-000000000000", 36, u));
    CuAssertIntEquals(tc, 0, uuid_version(u));
    CuAssertTrue(tc, 0 == uuid_v7_ms(u));
    uuid_tostr(u, s);
    CuAssertStrEquals(tc, "00000000-0000-0000-0000-000000000000", s);
    CuAssertIntEquals(tc, ERR_OK, uuid_fromstr("FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF", 36, u));
    CuAssertIntEquals(tc, 0, uuid_version(u));
    CuAssertTrue(tc, 0 == uuid_v7_ms(u));
    uuid_tostr(u, s);
    CuAssertStrEquals(tc, "ffffffff-ffff-ffff-ffff-ffffffffffff", s);
    // 版本位是 7 但变体位不是 10：同样不认
    CuAssertIntEquals(tc, ERR_OK, uuid_fromstr("017f22e2-79b0-7cc3-18c4-dc0c0c07398f", 36, u));
    CuAssertIntEquals(tc, 0, uuid_version(u));
    CuAssertTrue(tc, 0 == uuid_v7_ms(u));
    CuAssertIntEquals(tc, ERR_OK, uuid_fromstr("017f22e2-79b0-7cc3-d8c4-dc0c0c07398f", 36, u));
    CuAssertIntEquals(tc, 0, uuid_version(u));
    CuAssertTrue(tc, 0 == uuid_v7_ms(u));
}
// 版本位、变体位固定，其余随机位 1000 次里 0 和 1 都出现过
static void test_uuid_version_bits(CuTest *tc) {
    unsigned char or4[UUID_LENS], and4[UUID_LENS], or7[UUID_LENS], and7[UUID_LENS];
    char a[UUID_LENS];
    char b[UUID_LENS];
    int32_t i, k;
    memset(or4, 0, sizeof(or4));
    memset(and4, 0xFF, sizeof(and4));
    memset(or7, 0, sizeof(or7));
    memset(and7, 0xFF, sizeof(and7));
    for (i = 0; i < 1000; i++) {
        CuAssertIntEquals(tc, ERR_OK, uuid_v4(a));
        CuAssertIntEquals(tc, 4, uuid_version(a));
        CuAssertIntEquals(tc, ERR_OK, uuid_v7(b));
        CuAssertIntEquals(tc, 7, uuid_version(b));
        for (k = 0; k < UUID_LENS; k++) {
            or4[k] |= (unsigned char)a[k];
            and4[k] &= (unsigned char)a[k];
            or7[k] |= (unsigned char)b[k];
            and7[k] &= (unsigned char)b[k];
        }
    }
    // v4：只有第 6 字节高 4 位（0100）和第 8 字节高 2 位（10）固定
    for (k = 0; k < UUID_LENS; k++) {
        CuAssertIntEquals(tc, 6 == k ? 0x4F : (8 == k ? 0xBF : 0xFF), or4[k]);
        CuAssertIntEquals(tc, 6 == k ? 0x40 : (8 == k ? 0x80 : 0x00), and4[k]);
    }
    // v7：rand_b 是第 8 字节低 6 位加第 9~15 字节
    CuAssertIntEquals(tc, 0x70, or7[6] & 0xF0);
    CuAssertIntEquals(tc, 0x70, and7[6] & 0xF0);
    for (k = 8; k < UUID_LENS; k++) {
        CuAssertIntEquals(tc, 8 == k ? 0xBF : 0xFF, or7[k]);
        CuAssertIntEquals(tc, 8 == k ? 0x80 : 0x00, and7[k]);
    }
    // 两次 v4 相同的概率是 2^-122
    CuAssertIntEquals(tc, ERR_OK, uuid_v4(a));
    CuAssertIntEquals(tc, ERR_OK, uuid_v4(b));
    CuAssertTrue(tc, 0 != memcmp(a, b, UUID_LENS));
}
// 单线程连续 10000 个 v7 按 memcmp 严格递增，毫秒夹在生成前后的 nowms 之间
static void test_uuid_v7_monotonic(CuTest *tc) {
    const int32_t n = 10000;
    char *ids;
    uint64_t t0, t1, ms;
    int32_t i;
    MALLOC(ids, (size_t)n * UUID_LENS);
    t0 = nowms();
    for (i = 0; i < n; i++) {
        CuAssertIntEquals(tc, ERR_OK, uuid_v7(ids + (size_t)i * UUID_LENS));
    }
    t1 = nowms();
    for (i = 0; i < n; i++) {
        if (i > 0) {
            CuAssertTrue(tc, memcmp(ids + (size_t)(i - 1) * UUID_LENS, ids + (size_t)i * UUID_LENS, UUID_LENS) < 0);
        }
        ms = uuid_v7_ms(ids + (size_t)i * UUID_LENS);
        // 上界：同一毫秒超过 2048 个才会把时间戳推前，每 4096 个推 1ms；兜住进位算错
        CuAssertTrue(tc, t0 <= ms && ms <= t1 + (uint64_t)n / 2048 + 1);
    }
    FREE(ids);
}
static void _uuid_mt_gen(void *arg) {
    _uuid_mt_arg *a = (_uuid_mt_arg *)arg;
    int32_t i;
    for (i = 0; i < a->n; i++) {
        if (ERR_OK != uuid_v7(a->ids + (size_t)i * UUID_LENS)) {
            a->nfail++;
        }
    }
}
// 按前 8 字节（毫秒、版本、计数器）比较
static int _uuid_cmp8(const void *a, const void *b) {
    return memcmp(a, b, 8);
}
// 多线程：各线程内部严格递增；合并后前 8 字节没有重复，即 CAS 没把同一个 (毫秒, 计数器) 发给两个线程
static void test_uuid_v7_threads(CuTest *tc) {
    _uuid_mt_arg args[UUID_MT_THREADS];
    pthread_t th[UUID_MT_THREADS];
    size_t per = (size_t)UUID_MT_PER * UUID_LENS;
    size_t total = (size_t)UUID_MT_PER * UUID_MT_THREADS;
    char *all;
    size_t i;
    int32_t t;
    MALLOC(all, per * UUID_MT_THREADS);
    for (t = 0; t < UUID_MT_THREADS; t++) {
        args[t].n = UUID_MT_PER;
        args[t].nfail = 0;
        args[t].ids = all + per * (size_t)t;
        th[t] = thread_creat(_uuid_mt_gen, &args[t]);
    }
    for (t = 0; t < UUID_MT_THREADS; t++) {
        thread_join(th[t]);
    }
    for (t = 0; t < UUID_MT_THREADS; t++) {
        CuAssertIntEquals(tc, 0, args[t].nfail);
        for (i = 1; i < (size_t)UUID_MT_PER; i++) {
            CuAssertTrue(tc, memcmp(args[t].ids + (i - 1) * UUID_LENS, args[t].ids + i * UUID_LENS, UUID_LENS) < 0);
        }
    }
    qsort(all, total, UUID_LENS, _uuid_cmp8);
    for (i = 1; i < total; i++) {
        CuAssertTrue(tc, memcmp(all + (i - 1) * UUID_LENS, all + i * UUID_LENS, 8) < 0);
    }
    FREE(all);
}

/* =======================================================================
 * hash_ring —— 一致性哈希
 * ======================================================================= */
// hash_ring_add 只排新副本再归并：逐个 add 与 add_nosort + 整体 sort 建出的环必须一致；
// 副本名 "节点名-序号" 手写拼接，落点钉死为改写前算出的值(名字差一个字节 digest 就变)
static void test_hash_ring_incremental(CuTest *tc) {
    static const char *expect[12] = { "node3", "node15", "node2", "node19", "node10", "node12",
                                      "node2", "node5", "node14", "node1", "node15", "node4" };
    hash_ring_ctx a, b;
    char name[700], key[16];
    int32_t i, len;
    hash_ring_node *na, *nb;
    hash_ring_init(&a);
    hash_ring_init(&b);
    for (i = 0; i < 20; i++) {
        len = snprintf(name, sizeof(name), "node%d", i);
        CuAssertIntEquals(tc, ERR_OK, hash_ring_add(&a, name, (size_t)len, 160));
        CuAssertIntEquals(tc, ERR_OK, hash_ring_add_nosort(&b, name, (size_t)len, 160));
    }
    for (i = 0; i < 12; i++) {
        len = snprintf(key, sizeof(key), "key%d", i);
        na = hash_ring_find(&a, key, (size_t)len);
        CuAssertTrue(tc, strlen(expect[i]) == na->lens && 0 == memcmp(expect[i], na->name, na->lens));
    }
    // 再加一个超长名(走堆分支)与副本数各异的节点，两种建法仍须一致
    memset(name, 'L', 600);
    CuAssertIntEquals(tc, ERR_OK, hash_ring_add(&a, name, 600, 77));
    CuAssertIntEquals(tc, ERR_OK, hash_ring_add_nosort(&b, name, 600, 77));
    CuAssertIntEquals(tc, ERR_OK, hash_ring_add(&a, (void *)"tail", 4, 1));
    CuAssertIntEquals(tc, ERR_OK, hash_ring_add_nosort(&b, (void *)"tail", 4, 1));
    hash_ring_sort(&b);
    CuAssertIntEquals(tc, (int32_t)b.nitems, (int32_t)a.nitems);
    for (i = 0; i < 5000; i++) {
        len = snprintf(key, sizeof(key), "user:%d", i);
        na = hash_ring_find(&a, key, (size_t)len);
        nb = hash_ring_find(&b, key, (size_t)len);
        CuAssertTrue(tc, na->lens == nb->lens && 0 == memcmp(na->name, nb->name, na->lens));
    }
    hash_ring_free(&a);
    hash_ring_free(&b);
}
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
    /* free 后必须重新 init 才能复用：今天能跑通只因 hash_ring_free 末尾顺手调了
       hash_ring_init，而 hash_ring.h 明确写了那次复位只为"重复释放不崩 + 释放后 find
       不解引用已置空的 items"，不是复用入口 —— 别把实现细节当契约用 */
    hash_ring_init(&ring);

    /* 环定位：2 节点各 150 副本，200 个固定 key 两个节点都得命中过。
       哈希分布可重现，但不钉"哪个 key 落哪个节点"；二分退化成恒取 items[0]
       时全部 key 会挤到同一个节点上 */
    char skey[16];
    int32_t nhit_a = 0, nhit_b = 0;
    hash_ring_node *hit;
    CuAssertIntEquals(tc, ERR_OK, hash_ring_add(&ring, (void *)"ringA", 5, 150));
    CuAssertIntEquals(tc, ERR_OK, hash_ring_add(&ring, (void *)"ringB", 5, 150));
    for (int i = 0; i < 200; i++) {
        SNPRINTF(skey, sizeof(skey), "spread-%d", i);
        hit = hash_ring_find(&ring, skey, strlen(skey));
        CuAssertPtrNotNull(tc, hit);
        CuAssertTrue(tc, 5 == hit->lens);
        if (0 == memcmp(hit->name, "ringA", 5)) {
            nhit_a++;
        } else {
            CuAssertTrue(tc, 0 == memcmp(hit->name, "ringB", 5));
            nhit_b++;
        }
    }
    CuAssertTrue(tc, nhit_a > 0);
    CuAssertTrue(tc, nhit_b > 0);

    /* 摘掉一个节点后，同一批 key 只能落到剩下那个 */
    hash_ring_remove(&ring, (void *)"ringA", 5);
    for (int i = 0; i < 200; i++) {
        SNPRINTF(skey, sizeof(skey), "spread-%d", i);
        hit = hash_ring_find(&ring, skey, strlen(skey));
        CuAssertPtrNotNull(tc, hit);
        CuAssertTrue(tc, 5 == hit->lens && 0 == memcmp(hit->name, "ringB", 5));
    }

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
    binary_init_write(&bw, 0, 32);
    binary_set_va(&bw, "val=%d", 42);
    /* binary_set_va 写入 "val=42\0"，offset 停在 '\0' 前 */
    CuAssertTrue(tc, 6 == (int)bw.offset);
    CuAssertTrue(tc, 0 == memcmp("val=42", bw.data, 6));
    binary_free(&bw);

    /* ── binary_at：按位置取指针 ── */
    binary_init_write(&bw, 0, 32);
    binary_set_int8(&bw, 'A');
    binary_set_int8(&bw, 'B');
    binary_set_int8(&bw, 'C');
    CuAssertTrue(tc, 'A' == *binary_at(&bw, 0));
    CuAssertTrue(tc, 'B' == *binary_at(&bw, 1));
    CuAssertTrue(tc, 'C' == *binary_at(&bw, 2));
    binary_free(&bw);

    /* ── binary_offset (回填模式)：先占位，写内容后回到占位处回填 ── */
    binary_init_write(&bw, 0, 64);
    binary_set_skip(&bw, 4);/* 预留 4 字节长度字段 */
    size_t body_start = bw.offset;
    binary_set_binary(&bw, "body", 4);/* 写入消息体（4 字节，无 \0）*/
    size_t body_end = bw.offset;
    size_t body_len = body_end - body_start;/* 4 */

    binary_offset(&bw, 0);/* 绝对定位到起始 */
    binary_set_integer(&bw, (int64_t)body_len, 4, 0); /* 回填大端序长度 */
    binary_offset(&bw, body_end);/* 恢复到末尾 */

    /* 验证：从头读取长度字段和消息体 */
    binary_ctx br;
    binary_init_read(&br, bw.data, body_end);
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

    /* end 是闭区间，且要求整个 what 落在 [start, end] 内。
       单字节 needle 起点即末字节，看不出这条规则，另用 2 字节的钉一遍：
       "aaa|bbb|ccc" 里 "bb" 占 4-5，end=5 命中、end=4 不命中（起点在界内但末字节越界） */
    CuAssertTrue(tc, 3 == buffer_search(&buf, 0, 0, 3, "|", 1));
    CuAssertTrue(tc, ERR_FAILED == buffer_search(&buf, 0, 0, 2, "|", 1));
    CuAssertTrue(tc, 4 == buffer_search(&buf, 0, 0, 5, "bb", 2));
    CuAssertTrue(tc, ERR_FAILED == buffer_search(&buf, 0, 0, 4, "bb", 2));

    /* drain 请求量超出 buffer 大小时，仅删除实际数据 */
    size_t drained = buffer_drain(&buf, 1000);
    CuAssertTrue(tc, 11 == (int)drained);
    CuAssertTrue(tc, 0 == buffer_size(&buf));

    /* copyout 请求量超出时，返回实际可读字节数 */
    buffer_append(&buf, "hello", 5);
    char out[100];// 容量须够请求量：可读的若真有 100 字节就会全拷进来
    size_t nr = buffer_copyout(&buf, 0, out, sizeof(out));
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
 * buffer_from_sock 的读循环按事件后端分两档，这里两档都测。
 * 早退档(TRIGGER_LT 的四套后端 + IOCP)：没读满即 socket 已空，那轮必然 EAGAIN 的
 * 确认读整个省掉，剩下的由下次可读事件领走。IOCP 能同档是因为它的 ol_r 投 0 字节
 * WSARecv 探针，socket 还有数据时重投立即完成，与电平同效。
 * 不早退档(epoll 边缘触发，TRIGGER_ET=1)：ET 必须读到 EAGAIN，那一轮省不掉；
 * 但它大概率直接 EAGAIN，没必要为它再要一个 MAX_RECV_SIZE 的新节点 ——
 * 改为只用 buffer_space 报出的现成余量。读满的那轮说明还有数据，仍按
 * MAX_RECV_SIZE 取，否则大流量下每轮只读几百字节，readv 次数翻倍。
 * 读满的那轮两档行为一致，故场景二不分档
 * ======================================================================= */
static size_t _fake_rv_want[FAKE_RV_MAX];// 第 i 次调用要吐出的字节数
static size_t _fake_rv_offer[FAKE_RV_MAX];// 第 i 次调用被提供的 iov 总空间
static uint32_t _fake_rv_niov[FAKE_RV_MAX];// 第 i 次调用被提供的 iov 条数
static int32_t _fake_rv_calls;
static int32_t _fake_rv_fail_at;// 第几次调用返 ERR_FAILED(1 起算),0 为一路成功
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
        _fake_rv_niov[_fake_rv_calls] = niov;
        remain = _fake_rv_want[_fake_rv_calls];
    }
    _fake_rv_calls++;
    if (_fake_rv_calls == _fake_rv_fail_at) {
        *readed = 0;// 失败也得写出参: 调用方拿它去 commit_expand
        return ERR_FAILED;
    }
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
    memset(_fake_rv_niov, 0, sizeof(_fake_rv_niov));
    _fake_rv_calls = 0;
    _fake_rv_fail_at = 0;
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
#if defined(TRIGGER_LT) || defined(EV_IOCP)
    // 早退档：没读满就认定 socket 已空，那轮必然 EAGAIN 的确认读被整个省掉
    CuAssertIntEquals(tc, 1, _fake_rv_calls);
#else
    // 确认轮确实发生了 —— 不能靠"不读"来省开销，那会违反 ET 契约
    CuAssertIntEquals(tc, 2, _fake_rv_calls);
    // 确认轮只拿首节点写剩的余量，不为它新建节点 —— 这一条才是被测行为本身
    CuAssertTrue(tc, _fake_rv_offer[1] < MAX_RECV_SIZE);
#endif
    // 首轮无历史可依，仍按 MAX_RECV_SIZE 要空间
    CuAssertTrue(tc, _fake_rv_offer[0] >= MAX_RECV_SIZE);
    // 首节点是为 MAX_RECV_SIZE 建的，写掉 2000 后余量必然小于 MAX_RECV_SIZE；
    // 若确认轮又建了一个空节点，可写空间会被顶到 MAX_RECV_SIZE 以上
    CuAssertTrue(tc, buffer_space(&buf, MAX_EXPAND_NIOV) < MAX_RECV_SIZE);
    buffer_free(&buf);

#if defined(TRIGGER_LT) || defined(EV_IOCP)
    // 场景二(早退档)：填满就得接着读。want 给足让假 readv 把 iov 填满 ——
    // 这一条直接钉早退判据用的是本轮 iov 实际给出的总量(offer)而不是 nbuf：
    // expand 给出的空间大于 nbuf 时，用 nbuf 判会把"填满了"误当"读空了"，每轮少读一截
    buffer_init(&buf);
    _fake_rv_reset();
    _fake_rv_want[0] = MAX_RECV_SIZE * 4;
    _fake_rv_want[1] = MAX_RECV_SIZE * 4;
    CuAssertIntEquals(tc, ERR_OK, buffer_from_sock(&buf, 0, &nread, _fake_readv, NULL));
    // 前两轮都被填满故不早退，第三轮吐 0 才停
    CuAssertIntEquals(tc, 3, _fake_rv_calls);
    CuAssertTrue(tc, nread == _fake_rv_offer[0] + _fake_rv_offer[1]);
    CuAssertTrue(tc, nread == buffer_size(&buf));
    // 每轮给出的空间都不小于 MAX_RECV_SIZE：读满的那轮说明还有数据，不该缩水
    CuAssertTrue(tc, _fake_rv_offer[0] >= MAX_RECV_SIZE);
    CuAssertTrue(tc, _fake_rv_offer[1] >= MAX_RECV_SIZE);
    CuAssertTrue(tc, _fake_rv_offer[2] >= MAX_RECV_SIZE);
    buffer_free(&buf);
#else
    // 场景二(不早退档，即 epoll 边缘触发)：连续两轮读满，后续轮次仍须按 MAX_RECV_SIZE 要空间。
    // 这里 want 取 MAX_RECV_SIZE 而非填满 offer——该档不看 offer，读满与否只影响下轮 nbuf
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
#endif
}

/* =======================================================================
 * _readv 失败分支：失败码必须原样返回给调用方 —— usock.c 的 _usk_tcp_recv 靠它
 * 走 _evpub_mark_close，吞掉的话出错的 socket 永远关不掉。
 * 失败前已读到的字节也不许丢，仍要能 drain 出来
 * ======================================================================= */
static void test_buffer_from_sock_readv_fail(CuTest *tc) {
    buffer_ctx buf;
    size_t nread;

    // 首轮就失败：失败码上传，一个字节也没读到
    buffer_init(&buf);
    _fake_rv_reset();
    _fake_rv_fail_at = 1;
    CuAssertIntEquals(tc, ERR_FAILED, buffer_from_sock(&buf, 0, &nread, _fake_readv, NULL));
    CuAssertIntEquals(tc, 1, _fake_rv_calls);
    CuAssertTrue(tc, 0 == nread);
    CuAssertTrue(tc, 0 == buffer_size(&buf));
    buffer_free(&buf);

#if !defined(READV_EINVAL) && !defined(TRIGGER_LT) && !defined(EV_IOCP)
    // 短读一轮后确认轮失败：失败码照样上传，前一轮的 1500 字节完好可读。
    // 短读即 break 的那几档(READV_EINVAL / TRIGGER_LT / EV_IOCP)根本走不到确认轮，故只跳过这一段；
    // 上面"首轮就失败"那段与后端无关，照跑
    char readback[8];
    buffer_init(&buf);
    _fake_rv_reset();
    _fake_rv_want[0] = 1500;
    _fake_rv_fail_at = 2;
    CuAssertIntEquals(tc, ERR_FAILED, buffer_from_sock(&buf, 0, &nread, _fake_readv, NULL));
    CuAssertIntEquals(tc, 2, _fake_rv_calls);
    CuAssertTrue(tc, 1500 == nread);
    CuAssertTrue(tc, 1500 == buffer_size(&buf));
    CuAssertTrue(tc, sizeof(readback) == buffer_copyout(&buf, 0, readback, sizeof(readback)));
    CuAssertTrue(tc, 0 == memcmp("RRRRRRRR", readback, sizeof(readback)));
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

typedef struct { chan_ctx *ch; int items; int nfail; } _chan_arg;

// 任一次 chan_send 失败就记账并关掉通道：不关的话消费者会永远阻塞在 chan_recv 上，
// 回归表现成整个 ./bin/test 挂死而不是一条 FAIL
static void _chan_sender(void *arg) {
    _chan_arg *a = (_chan_arg *)arg;
    uintptr_t v;
    int i;
    for (i = 1; i <= a->items; i++) {
        v = (uintptr_t)i;
        if (ERR_OK != chan_send(a->ch, (void *)v, 0, 0)) {
            a->nfail++;
            chan_close(a->ch);
            return;
        }
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
    size_t lens = 0;
    for (uintptr_t i = 1; i <= 3; i++) {
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
    lens = 0;
    CuAssertTrue(tc, NULL == chan_recv(ch, &lens));

    chan_free(ch);

    /* ── 并发：生产者线程发 1000 项，主线程收，验证总和 ── */
    ch = chan_init(64);
    _chan_arg arg = { ch, 1000, 0 };
    pthread_t tid = thread_creat(_chan_sender, &arg);

    int64_t sum = 0;
    int nrecv = 0;
    int i;
    void *p;
    for (i = 0; i < 1000; i++) {
        p = chan_recv(ch, &lens);
        if (NULL == p) {
            break;/* 生产者出错时会 close，这里才有出口 */
        }
        nrecv++;
        sum += (int64_t)(uintptr_t)p;
    }
    thread_join(tid);
    CuAssertIntEquals(tc, 0, arg.nfail);
    CuAssertIntEquals(tc, 1000, nrecv);
    /* 1+2+...+1000 = 500500 */
    CuAssertTrue(tc, 500500LL == sum);

    chan_free(ch);
}
// 缓冲 chan 的容量向上取到 2 的幂、最小 2:按 can_send 数能写几条(不会卡在 send 里),
// 写满后不可再发,再按 FIFO 读空,读空后不可再收
static void _chan_cap_check(CuTest *tc, uint32_t cap, uint32_t want) {
    chan_ctx *ch = chan_init(cap);
    uintptr_t n = 0, i;
    size_t lens;
    while (chan_can_send(ch) && n < 1024) {// 1024 只是防取整出错时死循环
        n++;
        CuAssertIntEquals(tc, ERR_OK, chan_send(ch, (void *)n, 0, 0));
    }
    CuAssertIntEquals(tc, (int)want, (int)n);
    CuAssertIntEquals(tc, (int)want, (int)chan_size(ch));
    CuAssertIntEquals(tc, 0, chan_can_send(ch));
    for (i = 1; i <= n; i++) {
        CuAssertTrue(tc, (void *)i == chan_recv(ch, &lens));
    }
    CuAssertIntEquals(tc, 0, chan_can_recv(ch));
    chan_free(ch);
}
static void test_chan_capacity_round(CuTest *tc) {
    _chan_cap_check(tc, 1, 2);
    _chan_cap_check(tc, 3, 4);
    _chan_cap_check(tc, 5, 8);
    _chan_cap_check(tc, 100, 128);
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

    /* timer_elapsed 纳秒值与 elapsed_ms 毫秒值量级一致。必须先睡够一段可测的时长：
       start 后立即读 e_ms 恒为 0，量级断言退化成 e_ns >= 0 的恒真式 */
    timer_start(&t);
    MSLEEP(20);
    e_ms = timer_elapsed_ms(&t);
    uint64_t e_ns = timer_elapsed(&t);
    CuAssertTrue(tc, e_ms >= 15);/* 下界给 5ms 余量，覆盖各平台 MSLEEP 粒度 */
    CuAssertTrue(tc, e_ms < 5000);/* 上界留宽，只挡换算因子量纲错位 */
    /* e_ns 后读，故必然不小于 e_ms 换算出的纳秒值，不需要余量 */
    CuAssertTrue(tc, e_ns >= (uint64_t)e_ms * 1000000ULL);
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

// timer_cur 的刻度换算(macOS ARM64 的 125/3、Windows 的 QPC 频率走乘移位)：拿 nowms 当尺子，
// 200ms 里两边走的差须在 ±10% 内。上面那组只挡 4 倍以上的错，换算多一倍(移位少 1)也照样过
static void test_timer_ratio(CuTest *tc) {
    timer_ctx t;
    timer_init(&t);
    uint64_t t0 = timer_cur(&t);
    uint64_t w0 = nowms();
    MSLEEP(200);
    uint64_t dt = (timer_cur(&t) - t0) / 1000000ULL;
    uint64_t dw = nowms() - w0;
    CuAssertTrue(tc, dw >= 150);
    CuAssertTrue(tc, dt * 10 >= dw * 9);
    CuAssertTrue(tc, dt * 10 <= dw * 11);
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
    if (diff < 0) {
        diff = -diff;
    }
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

    /* parse_svid + serviceid 往返：svid 是进程级全局，中途断言失败会 longjmp 跳过还原，
     * 把后面每一个用例的 createid 都污染掉。观测值先攒进局部量，还原之后再一起判 */
    uint16_t saved = parse_svid(createid());
    int32_t r_set42 = serviceid(0x42);
    uint16_t got42 = parse_svid(createid());
    int32_t r_back1 = serviceid(saved);
    uint16_t got_back1 = parse_svid(createid());
    int32_t r_setmax = serviceid(SERVICEID_MAX);
    uint16_t gotmax = parse_svid(createid());
    int32_t r_back2 = serviceid(saved);
    int32_t r_over = serviceid(SERVICEID_MAX + 1);
    int32_t r_ffff = serviceid(0xFFFF);
    uint16_t got_final = parse_svid(createid());

    CuAssertIntEquals(tc, ERR_OK, r_set42);
    CuAssertIntEquals(tc, 0x42, got42);
    CuAssertIntEquals(tc, ERR_OK, r_back1);
    CuAssertIntEquals(tc, saved, got_back1);
    /* 上界恰好是 SERVICEID_MAX，再大一个即拒 */
    CuAssertIntEquals(tc, ERR_OK, r_setmax);
    CuAssertIntEquals(tc, SERVICEID_MAX, gotmax);
    CuAssertIntEquals(tc, ERR_OK, r_back2);
    CuAssertIntEquals(tc, ERR_FAILED, r_over);
    CuAssertIntEquals(tc, ERR_FAILED, r_ffff);
    CuAssertIntEquals(tc, saved, got_final);/* 被拒的两次没改写 svid */

    /* procscnt：至少 1 个逻辑核心 */
    CuAssertTrue(tc, procscnt() >= 1);

    /* nowms / nowsec：非零且量级一致（ms >= sec × 1000）。
       sec 必须先取：反过来的话两次取钟之间跨过秒边界，sec 就比 ms 新一档，断言假失败 */
    uint64_t sec = nowsec();
    uint64_t ms = nowms();
    CuAssertTrue(tc, ms > 0);
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

    /* 查表用的是 STRICMP，扩展名大小写不敏感：三种写法必须给出同一个 content-type。
       唯一生产调用方传的是附件后缀，不做大小写规范化 */
    CuAssertStrEquals(tc, ct, contenttype(".HTML"));
    CuAssertStrEquals(tc, ct, contenttype(".Html"));

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
    // 先收拾再断言，同下面几段：夹在中间的失败会 longjmp 掉 popen_free，留下没回收的子进程
    int32_t no_pipe_ok = (ERR_OK == popen_startup(&ctx, cmd, NULL));
    int32_t no_pipe_wait = (ERR_OK == popen_waitexit(&ctx, 3000));
    int32_t no_pipe_code = popen_exitcode(&ctx);
    popen_free(&ctx);
    CuAssertTrue(tc, 0 != no_pipe_ok);
    CuAssertTrue(tc, 0 != no_pipe_wait);
    CuAssertIntEquals(tc, 0, no_pipe_code);

    /* 2. 只读模式：脚本输出固定字符串，验证读到 "hello popen" */
#ifdef OS_WIN
    SNPRINTF(cmd, sizeof(cmd), "cmd /c \"\"%s\" r\"", script);
#else
    SNPRINTF(cmd, sizeof(cmd), "sh \"%s\" r", script);
#endif
    int32_t start_ok = (ERR_OK == popen_startup(&ctx, cmd, "r"));
    int32_t wait_ok = (ERR_OK == popen_waitexit(&ctx, 3000));
    ZERO(buf, sizeof(buf));
    n = popen_read(&ctx, buf, sizeof(buf) - 1, NULL);
    int32_t hit_ok = (NULL != strstr(buf, "hello popen"));
    popen_free(&ctx);
    CuAssertTrue(tc, 0 != start_ok);
    CuAssertTrue(tc, 0 != wait_ok);
    CuAssertTrue(tc, n > 0);
    CuAssertTrue(tc, 0 != hit_ok);

    /* 3. 读写模式：写入一行，等待脚本回显，验证读回内容一致 */
#ifdef OS_WIN
    SNPRINTF(cmd, sizeof(cmd), "cmd /c \"\"%s\" rw\"", script);
#else
    SNPRINTF(cmd, sizeof(cmd), "sh \"%s\" rw", script);
#endif
    const char *msg = "srey test\n";
    start_ok = (ERR_OK == popen_startup(&ctx, cmd, "rw"));
    int32_t nwrite = popen_write(&ctx, msg, strlen(msg));
    wait_ok = (ERR_OK == popen_waitexit(&ctx, 3000));
    ZERO(buf, sizeof(buf));
    n = popen_read(&ctx, buf, sizeof(buf) - 1, NULL);
    hit_ok = (NULL != strstr(buf, "srey test"));
    popen_free(&ctx);
    CuAssertTrue(tc, 0 != start_ok);
    CuAssertTrue(tc, nwrite > 0);
    CuAssertTrue(tc, 0 != wait_ok);
    CuAssertTrue(tc, n > 0);
    CuAssertTrue(tc, 0 != hit_ok);

    start_ok = (ERR_OK == popen_startup(&ctx, cmd, "rw"));
    nwrite = popen_write(&ctx, msg, strlen(msg));
    wait_ok = (ERR_OK == popen_waitexit(&ctx, 3000));
    // 第二次 popen_free 是被测对象不是收尾，断言一律排在它后面：夹在两次 free 之间的话，
    // 前面任一条挂掉就 longjmp 走了，重复释放这条唯一的覆盖跟着一起丢
    popen_free(&ctx);
    popen_free(&ctx);
    CuAssertTrue(tc, 0 != start_ok);
    CuAssertTrue(tc, nwrite > 0);
    CuAssertTrue(tc, 0 != wait_ok);
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
    popen_close(&ctx);
    int32_t code = popen_exitcode(&ctx);
    popen_free(&ctx);
    CuAssertIntEquals(tc, 1, eof);
    CuAssertIntEquals(tc, 7, code);

    CuAssertIntEquals(tc, ERR_OK, popen_startup(&ctx, "sleep 30", "w"));
    size_t big = ONEK * ONEK;
    char *payload;
    MALLOC(payload, big);
    memset(payload, 'x', big);
    n = popen_write(&ctx, payload, big);
    FREE(payload);
    // 必须 close 再 free：kill + waitpid 在 close 里，free 只关 socket。
    // 少这一句，sleep 30 会活过本用例成为没人回收的子进程，而 ./bin/test 要阻塞等 SIGINT，
    // 它就一直挂到整个会话结束，每跑一轮再漏一个。断言同理必须排在这两句之后：CuAssert 走 longjmp
    popen_close(&ctx);
    popen_free(&ctx);
    CuAssertTrue(tc, n > 0);
    CuAssertTrue(tc, (size_t)n < big);// 一次写不完，返回部分
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

// 日志文件模式：slog_parts 与 slog 同内容输出相同(含堆分配与段内 NUL)；攒批写满中途回刷、
// 超过攒批缓冲(64KB)的长行回退直写、log_free 退出补刷，行数与先后都不能乱。借全局日志用，测完换回控制台
static void test_log_file_mode(CuTest *tc) {
    const char *path = "log_file_mode.tmp";
    const int32_t nbatch = 200;
    const size_t nlong = 70000;
    log_level prev = log_getlv();
    FILE *f = fopen(path, "wb+");
    char fill[600], head[16];
    char *big, *all, *cur, *eol, *msg;
    const char *parts[2];
    size_t lens[2], i;
    long flen;
    int32_t k, nline = 0;
    CuAssertPtrNotNull(tc, f);
    MALLOC(big, nlong);
    memset(fill, 'y', sizeof(fill));
    memset(big, 'z', nlong);
    log_free();
    log_init(f, 0);
    log_setlv(LOGLV_DEBUG);
    slog(LOGLV_INFO, "S:%s|%d", "abc", 7);
    parts[0] = "S:abc";
    lens[0] = 5;
    parts[1] = "|7";
    lens[1] = 2;
    slog_parts(LOGLV_INFO, parts, lens, 2);
    slog(LOGLV_INFO, "H:%.300s", fill);// 总长 302 >= 256，两边都走堆
    parts[0] = "H:";
    lens[0] = 2;
    parts[1] = fill;
    lens[1] = 300;
    slog_parts(LOGLV_INFO, parts, lens, 2);
    parts[1] = "ab\0cd";
    lens[1] = 5;
    parts[0] = "N:";
    slog_parts(LOGLV_INFO, parts, lens, 2);// 截在第一个 NUL 处
    for (k = 0; k < nbatch; k++) {// 200 x 600B 一批装不进 64KB，中途必回刷
        SNPRINTF(head, sizeof(head), "B%03d:", k);
        parts[0] = head;
        lens[0] = strlen(head);
        parts[1] = fill;
        lens[1] = sizeof(fill);
        slog_parts(LOGLV_INFO, parts, lens, 2);
    }
    parts[0] = "L:";
    lens[0] = 2;
    parts[1] = big;
    lens[1] = nlong;
    slog_parts(LOGLV_INFO, parts, lens, 2);
    log_free();
    log_init(NULL, 0);
    log_setlv(prev);
    FREE(big);

    fseek(f, 0, SEEK_END);
    flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    MALLOC(all, (size_t)flen + 1);
    CuAssertIntEquals(tc, (int32_t)flen, (int32_t)fread(all, 1, (size_t)flen, f));
    all[flen] = '\0';
    fclose(f);
    remove(path);
    cur = all;
    while (NULL != (eol = strchr(cur, '\n'))) {
        *eol = '\0';
        msg = strstr(cur, "][info]");
        CuAssertPtrNotNull(tc, msg);
        msg += 7;
        if (nline < 2) {
            CuAssertStrEquals(tc, "S:abc|7", msg);
        } else if (nline < 4) {
            CuAssertIntEquals(tc, 302, (int32_t)strlen(msg));
            CuAssertTrue(tc, 'H' == msg[0] && ':' == msg[1] && 'y' == msg[2] && 'y' == msg[301]);
        } else if (4 == nline) {
            CuAssertStrEquals(tc, "N:ab", msg);
        } else if (nline < 5 + nbatch) {
            SNPRINTF(head, sizeof(head), "B%03d:", nline - 5);
            CuAssertIntEquals(tc, 0, memcmp(msg, head, 5));
            CuAssertIntEquals(tc, 5 + (int32_t)sizeof(fill), (int32_t)strlen(msg));
            CuAssertTrue(tc, 'y' == msg[5] && 'y' == msg[5 + sizeof(fill) - 1]);
        } else if (5 + nbatch == nline) {
            CuAssertIntEquals(tc, 2 + (int32_t)nlong, (int32_t)strlen(msg));
            for (i = 2; i < 2 + nlong && 'z' == msg[i]; i++) {
            }
            CuAssertTrue(tc, 'L' == msg[0] && 2 + nlong == i);
        } else {
            CuAssertPtrNotNull(tc, strstr(msg, "log thread exited."));
        }
        nline++;
        cur = eol + 1;
    }
    CuAssertTrue(tc, '\0' == *cur);
    FREE(all);
    CuAssertIntEquals(tc, 7 + nbatch, nline);
}
// slog 等级过滤路径：lv > _log_lv 时早返，既不入队也不分配
// 注：mpq 入队/丢弃路径已由 test_mpq_concurrent_mc 覆盖，slog 入队路径无需重复测试
static void test_log_slog_filter(CuTest *tc) {
    const int32_t nfilter = 100;
    log_level prev = log_getlv();
    char big[300];// 超过 LOG_INLINE_SIZE(256)，漏过过滤的话每条都要单独 MALLOC 一次
    int32_t i;
#if MEMORY_CHECK
    uint64_t alloc0, alloc1;
#endif
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    // 设到 FATAL（最高级，值 0）：所有 lv > 0 的 slog 都被过滤
    log_setlv(LOGLV_FATAL);
    CuAssertIntEquals(tc, LOGLV_FATAL, log_getlv());
#if MEMORY_CHECK
    mem_stat(&alloc0, NULL);
#endif
    for (i = 0; i < nfilter; i++) {
        slog(LOGLV_ERROR, "filtered error %d %s", i, big);
        slog(LOGLV_WARN, "filtered warn %d %s", i, big);
        slog(LOGLV_INFO, "filtered info %d %s", i, big);
        slog(LOGLV_DEBUG, "filtered debug %d %s", i, big);
    }
#if MEMORY_CHECK
    // 上界取总条数(4 * nfilter)的一半：过滤正常时增量近 0（日志线程另有零星分配），
    // 漏过过滤则是每条一次 MALLOC，两者差一个数量级
    mem_stat(&alloc1, NULL);
    CuAssertTrue(tc, alloc1 - alloc0 < (uint64_t)nfilter * 2);
#endif
    // 过滤路径不许碰日志级别；还原后必须精确回到原值
    CuAssertIntEquals(tc, LOGLV_FATAL, log_getlv());
    log_setlv(prev);
    CuAssertIntEquals(tc, prev, log_getlv());
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

    /* 日期类递归格式串要把 S_YEAR|S_MON|S_MDAY 报回外层，收尾段才算得出 tm_yday；
       少报一位这里就停在 0。年月日本身是子解析直接写进 tm 的，验不到这条通路。
       2024 是闰年，5-21 的 yday = 31+29+31+30+21-1 = 141 */
    ZERO(&tm, sizeof(tm));
    CuAssertPtrNotNull(tc, _strptime("2024-05-21", "%F", &tm));
    CuAssertIntEquals(tc, 141, tm.tm_yday);
    ZERO(&tm, sizeof(tm));
    CuAssertPtrNotNull(tc, _strptime("05/21/24", "%D", &tm));
    CuAssertIntEquals(tc, 141, tm.tm_yday);
    ZERO(&tm, sizeof(tm));
    CuAssertPtrNotNull(tc, _strptime("24/05/21", "%x", &tm));
    CuAssertIntEquals(tc, 141, tm.tm_yday);
    /* %c 一次报五位，wday 由 %a 直接写、yday 仍走收尾段 */
    ZERO(&tm, sizeof(tm));
    CuAssertPtrNotNull(tc, _strptime("Tue May 21 00:00:00 2024", "%c", &tm));
    CuAssertIntEquals(tc, 141, tm.tm_yday);
    CuAssertIntEquals(tc, 2,   tm.tm_wday);

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

    /* memcasecmp：只折 ASCII 字母，先折叠再比大小，按长度比(内嵌 NUL 照常往后比) */
    CuAssertIntEquals(tc, 0, memcasecmp("Content-Length", "content-LENGTH", 14));
    CuAssertIntEquals(tc, 0, memcasecmp("abc", "xyz", 0));
    CuAssertIntEquals(tc, 1, memcasecmp("A", "_", 1));/* 折成 'a'(0x61) 才比 '_'(0x5f) 大；不折叠直接比是 -1 */
    CuAssertIntEquals(tc, -1, memcasecmp("@", "`", 1));/* 紧挨字母两端的 0x40/0x60/0x5b/0x7b 不参与折叠 */
    CuAssertIntEquals(tc, -1, memcasecmp("[", "{", 1));
    CuAssertIntEquals(tc, 0, memcasecmp("a\0B", "A\0b", 3));
    CuAssertIntEquals(tc, -1, memcasecmp("a\0b", "A\0c", 3));
    CuAssertIntEquals(tc, -1, memcasecmp("\xC1", "\xE1", 1));/* 0x80 以上原样比，不按任何 locale 折 */
    CuAssertIntEquals(tc, 1, memcasecmp("\xE1", "a", 1));/* 按无符号字节比 */

    /* STRICMP / STRNCMP：折叠规则同上，遇 NUL 结束；短串是长串前缀时短串小 */
    CuAssertIntEquals(tc, 0, STRICMP(".HTML", ".html"));
    CuAssertIntEquals(tc, -1, STRICMP(".htm", ".html"));
    CuAssertIntEquals(tc, 1, STRICMP(".json", ".JS"));
    CuAssertIntEquals(tc, 1, STRICMP("A", "_"));
    CuAssertIntEquals(tc, 0, STRICMP("", ""));
    CuAssertIntEquals(tc, 1, STRICMP("\xC1", "a"));
    CuAssertIntEquals(tc, 0, STRNCMP("Keep-Alive", "keep-alivexx", 10));
    CuAssertIntEquals(tc, 0, STRNCMP("ab\0x", "AB\0y", 4));/* 第 3 字节两边都是 NUL,到此为止 */
    CuAssertIntEquals(tc, -1, STRNCMP("ab", "abc", 3));
    CuAssertIntEquals(tc, 0, STRNCMP("abc", "xyz", 0));
    {
        /* 单字节全组合对照 ASCII 折叠规则；memichr 在 0..255 全表里找到的必须是第一个折叠后相等的位置 */
        unsigned char all[256];
        unsigned char ca, cb;
        int32_t a, b, fa, fb, want, bad = 0;
        const unsigned char *hit;
        for (a = 0; a < 256; a++) {
            all[a] = (unsigned char)a;
        }
        for (a = 0; a < 256; a++) {
            ca = (unsigned char)a;
            fa = (a >= 'A' && a <= 'Z') ? a + 32 : a;
            for (b = 0; b < 256; b++) {
                cb = (unsigned char)b;
                fb = (b >= 'A' && b <= 'Z') ? b + 32 : b;
                want = fa == fb ? 0 : (fa > fb ? 1 : -1);
                if (want != memcasecmp(&ca, &cb, 1)) {
                    bad++;
                }
            }
            hit = (const unsigned char *)memichr(all, a, sizeof(all));
            want = (a >= 'a' && a <= 'z') ? a - 32 : a;/* 全表里大写在前，小写字母先命中其大写 */
            if (NULL == hit || want != (int32_t)(hit - all)) {
                bad++;
            }
        }
        CuAssertIntEquals(tc, 0, bad);
    }

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

    /* tohex：二进制转 16 进制（大写）。缓冲先填非零，结束符由 tohex 自己写——
       测试预写 '\0' 的话，"tohex 不写结束符"这个回归就测不出来 */
    const uint8_t bin[] = { 0x00, 0xab, 0xff, 0x10 };
    char hex[HEX_ENSIZE(4)];
    memset(hex, 'Z', sizeof(hex));
    CuAssertTrue(tc, hex == tohex(bin, 4, hex, 0));
    CuAssertStrEquals(tc, "00ABFF10", hex);
    /* tohex：二进制转 16 进制（小写）*/
    char hexlower[HEX_ENSIZE(4)];
    memset(hexlower, 'Z', sizeof(hexlower));
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
 * hash / randrange / randstr / IS_LITTLE / timeoffset / fill_timespec / threadid
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
    /* 按 6 位切随机数、62 以外丢弃：只出字母数字，62 个字符都取得到(含表尾的 '8' '9') */
    char big[62 * 200 + 1];
    int32_t seen[256] = { 0 }, nseen = 0;
    randstr(big, sizeof(big) - 1);
    for (size_t ri = 0; ri < sizeof(big) - 1; ri++) {
        CuAssertTrue(tc, 0 != isalnum((unsigned char)big[ri]));
        if (0 == seen[(unsigned char)big[ri]]++) {
            nseen++;
        }
    }
    CuAssertIntEquals(tc, 62, nseen);

    /* IS_LITTLE：当前平台（macOS/Linux x86/ARM）均小端 */
    CuAssertIntEquals(tc, 1, IS_LITTLE);

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
    /* 起点偏移 0~8、长度 0~40：MSVC 分支先逐字节写到 8 字节对齐、中间按 8 字节写、余下逐字节，
       三段都要走到；范围内全清零，范围外一个字节都不能碰 */
    uint64_t al[12];
    unsigned char *zb = (unsigned char *)al;
    size_t off, zl, k;
    for (off = 0; off <= 8; off++) {
        for (zl = 0; zl <= 40; zl++) {
            memset(al, 0xAB, sizeof(al));
            secure_zero(zb + off, zl);
            for (k = 0; k < sizeof(al); k++) {
                CuAssertTrue(tc, (k >= off && k < off + zl) ? (0 == zb[k]) : (0xAB == zb[k]));
            }
        }
    }

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
            if (got >= (int)strlen(msg)) {
                break;
            }
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
            if (got >= (int)strlen(back)) {
                break;
            }
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
    SOCKET lsn = sock_create_cloexec(AF_INET, SOCK_STREAM, 0, 0);
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
    FILE *fp = fopen_cloexec(tmpfile, "wb");
    CuAssertPtrNotNull(tc, fp);
    size_t nwrite = fwrite(content, 1, clen, fp);
    // 底层描述符必须带"子进程不可继承"标记，否则 popen 起的 /bin/sh 能拿到它。
    // 判定要 fp 还活着，故先取到局部量，fclose 之后再断言（断言失败会 longjmp 跳过 fclose）
#ifdef OS_WIN
    DWORD hflag = 0;
    int32_t hok = (0 != GetHandleInformation((HANDLE)_get_osfhandle(_fileno(fp)), &hflag));
#else
    int32_t fdflag = fcntl(fileno(fp), F_GETFD);
#endif
    fclose(fp);
    CuAssertTrue(tc, clen == nwrite);
#ifdef OS_WIN
    CuAssertTrue(tc, hok);
    CuAssertTrue(tc, 0 == (hflag & HANDLE_FLAG_INHERIT));
#else
    CuAssertTrue(tc, -1 != fdflag);
    CuAssertTrue(tc, 0 != (fdflag & FD_CLOEXEC));
#endif

    CuAssertIntEquals(tc, ERR_OK, isfile(tmpfile));
    CuAssertTrue(tc, ERR_OK != isdir(tmpfile));
    CuAssertTrue(tc, (int64_t)clen == filesize(tmpfile));

    // 先收拾再断言：CuAssert 失败走 longjmp，夹在中间会漏掉 data
    size_t got = 0;
    char *data = readall(tmpfile, &got);
    int32_t data_ok = (NULL != data);
    int32_t len_ok = (clen == got);
    int32_t cmp_ok = (data_ok && 0 == memcmp(content, data, clen));
    FREE(data);
    CuAssertTrue(tc, 0 != data_ok);
    CuAssertTrue(tc, 0 != len_ok);
    CuAssertTrue(tc, 0 != cmp_ok);

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
    char *rdempty = readall(tmpfile, &got);
    int32_t empty_ok = (NULL == rdempty);
    int32_t errno_ok = (0 != errno);
    FREE(rdempty);// 回归成"空文件也返缓冲"时不漏，否则真失败之外还多报一笔 not free
    // 临时文件先删掉：断言排在后面，中途 longjmp 会把它留在 bin/ 下
    remove(tmpfile);
    CuAssertTrue(tc, 0 != empty_ok);
    CuAssertTrue(tc, 0 != errno_ok);
}

/* =======================================================================
 * popen_close —— 子进程未结束时强制终止并回收
 * Unix: SIGKILL + waitpid 收尸，ctx->exited=1 / exitcode=ERR_FAILED
 * Windows: 结束整个作业对象(子进程及其派生的孙进程)
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
#ifdef OS_WIN
    /* 作业对象里应是 cmd 与它派生的 timeout.exe：先等孙进程起来，否则 close 太早测不到它 */
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct;
    int32_t alive = -1, waits;
    CuAssertPtrNotNull(tc, ctx.job);
    for (waits = 0; waits < 500; waits++) {
        if (QueryInformationJobObject(ctx.job, JobObjectBasicAccountingInformation, &acct, sizeof(acct), NULL)
            && acct.ActiveProcesses >= 2) {
            break;
        }
        MSLEEP(10);
    }
    CuAssertTrue(tc, waits < 500);
#endif

    /* 立即 popen_close 应在毫秒级返回（SIGKILL + waitpid 同步收尸） */
    uint64_t t0 = nowms();
    popen_close(&ctx);
    uint64_t elapsed = nowms() - t0;
    CuAssertTrue(tc, elapsed < 5000); /* 5s 内必结束（实际应远低于 100ms） */
#ifdef OS_WIN
    /* 连孙进程一起结束：作业里的活进程数归零 */
    for (waits = 0; waits < 500; waits++) {
        if (QueryInformationJobObject(ctx.job, JobObjectBasicAccountingInformation, &acct, sizeof(acct), NULL)) {
            alive = (int32_t)acct.ActiveProcesses;
            if (0 == alive) {
                break;
            }
        }
        MSLEEP(10);
    }
    CuAssertIntEquals(tc, 0, alive);
#endif
#ifndef OS_WIN
    /* Unix 下 popen_close 自带 waitpid，exited 标志置 1 */
    CuAssertIntEquals(tc, 1, ctx.exited);
    CuAssertIntEquals(tc, ERR_FAILED, ctx.exitcode);
#endif
    popen_free(&ctx);

    /* close 后再次 close 应为 no-op（idempotent，不应 crash 不应阻塞） */
    CuAssertIntEquals(tc, ERR_OK, popen_startup(&ctx, cmd, NULL));
    popen_close(&ctx);
    popen_close(&ctx);/* 重复调用：pid 仍非 0 但 exited=1，分支 if 不进入 kill */
    popen_free(&ctx);
}

/* popen_waitexit 等退出事件(POSIX)：子进程一退出就返回并拿到退出码，不必等退避轮询的下一拍；
 * 超时那支照旧返 ERR_FAILED，ms=0 只探一次。时间上下界放得很宽，只挡"等满超时"这种回归 */
static void test_popen_waitexit_event(CuTest *tc) {
#ifndef OS_WIN
    popen_ctx ctx;
    uint64_t t0, cost;
    CuAssertIntEquals(tc, ERR_OK, popen_startup(&ctx, "sh -c 'sleep 0.2; exit 3'", NULL));
    t0 = nowms();
    CuAssertIntEquals(tc, ERR_OK, popen_waitexit(&ctx, 10000));
    cost = nowms() - t0;
    CuAssertTrue(tc, cost < 5000);
    CuAssertIntEquals(tc, 3, popen_exitcode(&ctx));
    popen_free(&ctx);

    CuAssertIntEquals(tc, ERR_OK, popen_startup(&ctx, "sh -c 'sleep 30'", NULL));
    CuAssertIntEquals(tc, ERR_FAILED, popen_waitexit(&ctx, 0));
    t0 = nowms();
    CuAssertIntEquals(tc, ERR_FAILED, popen_waitexit(&ctx, 150));
    cost = nowms() - t0;
    CuAssertTrue(tc, cost >= 140 && cost < 5000);
    popen_close(&ctx);
    popen_free(&ctx);
#else
    (void)tc;
#endif
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
    /* free 是本用例唯一的收尸口，断言不能夹在它前面：longjmp 会把 sleep 30 留成孤儿 */
    int32_t has_pid = (0 != ctx.pid);
    int32_t exited0 = ctx.exited;
#endif
    /* 故意跳过 popen_close，直接 free */
    uint64_t t0 = nowms();
    popen_free(&ctx);
    uint64_t cost = nowms() - t0;
#ifndef OS_WIN
    CuAssertTrue(tc, has_pid);
    CuAssertIntEquals(tc, 0, exited0);
#endif
    CuAssertTrue(tc, cost < 5000);/* 同步 SIGKILL + waitpid，不该等满 30 秒 */
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

    /* 未知转换符 → NULL（%X 是支持的，见 test_strptime_ampm_overflow；%Q 才不存在） */
    ZERO(&tm, sizeof(tm));
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

/* =======================================================================
 * %p 的"小时已定就不许再加 12"守卫读的是 state 里的 S_HOUR，而 %c/%R/%r/%T/%X
 * 走 goto recurse 另起一次 _strptime，内层置的位从不回传外层，守卫被静默跳过：
 * "%T %p" 喂 "13:00:00 PM" 会得到 tm_hour=25 且返回成功，下游 mktime 静默滚到第二天。
 * 每个写 tm_hour 的地方都补上置位后，本用例才成立
 * ======================================================================= */
static void test_strptime_ampm_overflow(CuTest *tc) {
    struct tm tm;
    char *end;

    /* 递归格式串解出的小时 > 11，后面再跟 %p 必须被挡下 */
    ZERO(&tm, sizeof(tm));
    CuAssertTrue(tc, NULL == _strptime("13:00:00 PM", "%T %p", &tm));
    ZERO(&tm, sizeof(tm));
    CuAssertTrue(tc, NULL == _strptime("13:00:00 PM", "%X %p", &tm));
    ZERO(&tm, sizeof(tm));
    CuAssertTrue(tc, NULL == _strptime("13:00 PM", "%R %p", &tm));
    ZERO(&tm, sizeof(tm));
    CuAssertTrue(tc, NULL == _strptime("Mon Jan  1 13:00:00 2024 PM", "%c %p", &tm));
    /* %r 自身就是 "%I:%M:%S %p"，内层加过的 12 不回传外层的话还能再加一次 */
    ZERO(&tm, sizeof(tm));
    CuAssertTrue(tc, NULL == _strptime("11:00:00 PM PM", "%r %p", &tm));
    /* %p 自己也要置位：连着两个 %p 不能累加两次 12 */
    ZERO(&tm, sizeof(tm));
    CuAssertTrue(tc, NULL == _strptime("PM PM", "%p %p", &tm));
    /* 直写 %H 的那条路本来就挡得住，作为对照 */
    ZERO(&tm, sizeof(tm));
    CuAssertTrue(tc, NULL == _strptime("13:00:00 PM", "%H:%M:%S %p", &tm));

    /* %s 经 localtime 把整个 tm 填掉，小时随本机时区变，所以先量出来再据此判定。
     * 两个时间戳相隔 12 小时，任何时区下必有一个落在 12 点之后，那一个才验得到守卫 */
    static const char *const epochs[] = { "1700000000", "1700043200" };
    char sinput[32];
    int32_t shour;
    size_t si;
    for (si = 0; si < sizeof(epochs) / sizeof(epochs[0]); si++) {
        ZERO(&tm, sizeof(tm));
        CuAssertPtrNotNull(tc, _strptime(epochs[si], "%s", &tm));
        shour = tm.tm_hour;
        SNPRINTF(sinput, sizeof(sinput), "%s PM", epochs[si]);
        ZERO(&tm, sizeof(tm));
        end = _strptime(sinput, "%s %p", &tm);
        if (shour > 11) {
            CuAssertTrue(tc, NULL == end);
        } else {
            CuAssertPtrNotNull(tc, end);
            CuAssertIntEquals(tc, shour + 12, tm.tm_hour);
        }
    }

    /* 合法输入不能被误挡：上午 + PM 仍要正常加 12 */
    ZERO(&tm, sizeof(tm));
    end = _strptime("01:00:00 PM", "%T %p", &tm);
    CuAssertPtrNotNull(tc, end);
    CuAssertIntEquals(tc, 13, tm.tm_hour);
    /* 不带 %p 的递归格式行为不变 */
    ZERO(&tm, sizeof(tm));
    end = _strptime("13:00:00", "%T", &tm);
    CuAssertPtrNotNull(tc, end);
    CuAssertIntEquals(tc, 13, tm.tm_hour);
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
 * pool —— 对象池:取/还/复用、满处理、收缩、释放(flags=0 queue / POOL_THSAFE fsqu)
 * ======================================================================= */
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
static void _pool_basic_check(CuTest *tc, int32_t flags) {
    pool_ctx pool;
    pool_t_obj *o, *o2;
    int32_t arg = 7;
    _pt_counters_reset();
    pool_init(&pool, sizeof(pool_t_obj), 8, 2, flags, &_pt_cbs);
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
    // per-object 计数:文件静态量给不出"是哪个对象被 reset/clear 了"——多对象在池时分不清。
    // 这两个字段原先只写不读,看着像在校验复用语义,实际什么都没校验
    CuAssertIntEquals(tc, 1, (int)o2->reset_cnt);
    CuAssertIntEquals(tc, 1, (int)o2->clear_cnt);
    CuAssertIntEquals(tc, 0, pool_size(&pool));
    pool_push(&pool, o2, 0);
    pool_free(&pool);
    CuAssertIntEquals(tc, 1, _pt_free);
    CuAssertIntEquals(tc, 0, _pt_free_bad);
}
static void test_pool_basic(CuTest *tc) {
    _pool_basic_check(tc, 0);
    _pool_basic_check(tc, POOL_THSAFE);
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
static void _pool_shrink_check(CuTest *tc, int32_t flags) {
    pool_ctx pool;
    pool_t_obj *objs[8];
    uint32_t i;
    _pt_counters_reset();
    pool_init(&pool, sizeof(pool_t_obj), 16, 2, flags, &_pt_cbs);
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
    _pool_shrink_check(tc, POOL_THSAFE); // 线程安全 fsqu —— 确认仍正确
}
// 收缩策略:nkeep 下限 与 load_trend busy 跳过(flags=0,确定性)
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
// 出队顺序:非安全池默认后进先出(拿刚还的),POOL_FIFO 与线程安全池先进先出。
// 收缩不论哪种都从最冷的队头释放,留下的是最近归还的那个
static void _pool_order_check(CuTest *tc, int32_t flags, int32_t lifo) {
    pool_ctx pool;
    pool_t_obj *a, *b, *c, *got, *left;
    _pt_counters_reset();
    pool_init(&pool, sizeof(pool_t_obj), 8, 0, flags, &_pt_cbs);
    a = (pool_t_obj *)pool_pop(&pool, NULL, 0);
    b = (pool_t_obj *)pool_pop(&pool, NULL, 0);
    c = (pool_t_obj *)pool_pop(&pool, NULL, 0);
    pool_push(&pool, a, 0);
    pool_push(&pool, b, 0);
    pool_push(&pool, c, 0);
    got = (pool_t_obj *)pool_pop(&pool, NULL, 0);
    CuAssertPtrEquals(tc, lifo ? c : a, got);
    pool_push(&pool, got, 0);// 还回队尾,成了最热的那个
    pool_shrink_to(&pool, 1);
    CuAssertIntEquals(tc, 2, _pt_free);
    CuAssertIntEquals(tc, 0, _pt_free_bad);
    CuAssertIntEquals(tc, 1, pool_size(&pool));
    left = (pool_t_obj *)pool_pop(&pool, NULL, 0);
    CuAssertPtrEquals(tc, got, left);
    pool_push(&pool, left, 0);
    pool_free(&pool);
    CuAssertIntEquals(tc, 3, _pt_free);
}
static void test_pool_order(CuTest *tc) {
    _pool_order_check(tc, 0, 1);
    _pool_order_check(tc, POOL_FIFO, 0);
    _pool_order_check(tc, POOL_THSAFE, 0);
    _pool_order_check(tc, POOL_THSAFE | POOL_FIFO, 0);// FIFO 对安全池不起作用,本来就先进先出
}
// 容量非 2 的幂时向上取到 2 的幂:写满 want 个后再还即失败(对象被释放),取空后再取走新建
static void _pool_cap_check(CuTest *tc, int32_t flags, uint32_t cap, uint32_t want) {
    pool_ctx pool;
    pool_t_obj *objs[128], *over;
    uint32_t i;
    _pt_counters_reset();
    pool_init(&pool, sizeof(pool_t_obj), cap, 0, flags, &_pt_cbs);
    CuAssertIntEquals(tc, (int)want, (int)pool_capacity(&pool));
    for (i = 0; i < want; i++) {
        objs[i] = (pool_t_obj *)pool_pop(&pool, NULL, 0);
    }
    for (i = 0; i < want; i++) {
        CuAssertIntEquals(tc, ERR_OK, pool_push(&pool, objs[i], 0));
    }
    CuAssertIntEquals(tc, (int)want, (int)pool_size(&pool));
    over = (pool_t_obj *)_pt_elnew(NULL);
    CuAssertIntEquals(tc, ERR_FAILED, pool_push(&pool, over, 0));
    CuAssertIntEquals(tc, 1, _pt_free);
    for (i = 0; i < want; i++) {
        objs[i] = (pool_t_obj *)pool_pop(&pool, NULL, 0);
    }
    CuAssertIntEquals(tc, 0, pool_size(&pool));
    CuAssertIntEquals(tc, (int)want + 1, (int)_pt_new);// want 个首次新建 + over,取空前没有多建
    over = (pool_t_obj *)pool_pop(&pool, NULL, 0);
    CuAssertIntEquals(tc, (int)want + 2, (int)_pt_new);// 已取空,再取只能新建
    _pt_elfree(over);
    for (i = 0; i < want; i++) {
        _pt_elfree(objs[i]);
    }
    pool_free(&pool);
    CuAssertIntEquals(tc, (int)want + 2, (int)_pt_free);
    CuAssertIntEquals(tc, 0, _pt_free_bad);
}
static void test_pool_capacity_round(CuTest *tc) {
    _pool_cap_check(tc, 0, 1, 2);// queue 最小 2
    _pool_cap_check(tc, 0, 3, 4);
    _pool_cap_check(tc, 0, 5, 8);
    _pool_cap_check(tc, 0, 100, 128);
    // 安全池只取 >= 8 的:bbq 后端最小容量就是 8,更小的值在它上面取整结果不同
    _pool_cap_check(tc, POOL_THSAFE, 5, 8);
    _pool_cap_check(tc, POOL_THSAFE, 100, 128);
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
    } else {
        // 一个候选 locale 都设不上时(精简容器/Alpine 常不带这些 locale 数据),上面只剩
        // "普通 strtod 也成立"的那条,strtod_c 存在的理由——不受 LC_NUMERIC 影响——本轮
        // 没被验证。打一行让人看出来,口径同 test_event.c 的证书缺失
        PRINT("skip strtod_c locale check, no comma-decimal locale available.");
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

/* =======================================================================
 * base.c 字符串 / 字节序辅助：与改写前的写法逐一对照
 * ======================================================================= */
// fromhex 查表前的写法
static int32_t _fromhex_ref(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return ERR_FAILED;
}
// 0..255 每个字节都与旧写法一致（含有符号 char 平台上的负值）
static void test_fromhex_table(CuTest *tc) {
    int32_t i, bad = 0;
    for (i = 0; i < 256; i++) {
        if (_fromhex_ref((char)i) != fromhex((char)i)) {
            bad++;
        }
    }
    CuAssertIntEquals(tc, 0, bad);
}

// str2u64 改 cut/lim 前的判溢出写法
static int32_t _str2u64_ref(const char *str, size_t lens, uint64_t max, uint64_t *out) {
    uint64_t v = 0;
    uint64_t d;
    size_t i;
    if (0 == lens
        || NULL == str) {
        return ERR_FAILED;
    }
    for (i = 0; i < lens; i++) {
        if (str[i] < '0'
            || str[i] > '9') {
            return ERR_FAILED;
        }
        d = (uint64_t)(str[i] - '0');
        if (d > max
            || v > (max - d) / 10) {
            return ERR_FAILED;
        }
        v = v * 10 + d;
    }
    *out = v;
    return ERR_OK;
}
// 新旧两版对同一输入：返回值一致，写出的值一致（失败时都不写）；*acc 累计新版收下的个数
static int32_t _str2u64_same(const char *s, size_t lens, uint64_t max, int32_t *acc) {
    uint64_t a = 0x5a5a5a5a;
    uint64_t b = 0x5a5a5a5a;
    int32_t ra = str2u64(s, lens, max, &a);
    int32_t rb = _str2u64_ref(s, lens, max, &b);
    if (NULL != acc && ERR_OK == ra) {
        (*acc)++;
    }
    return ra == rb && a == b;
}
// 十进制串原地加一，进位到头时在前面补 '1'（s 须多留一个字节）
static void _dec_inc(char *s) {
    size_t n = strlen(s);
    while (n > 0) {
        n--;
        if ('9' != s[n]) {
            s[n]++;
            return;
        }
        s[n] = '0';
    }
    memmove(s + 1, s, strlen(s) + 1);
    s[0] = '1';
}
static void test_str2u64_ref(CuTest *tc) {
    const uint64_t maxs[] = { 0, 1, 4, 9, 10, 11, 19, 20, 99, 100, 101, 255, 999, 1000, 65535,
        UINT32_MAX, (uint64_t)INT64_MAX, UINT64_MAX / 10, UINT64_MAX / 10 + 1, UINT64_MAX - 1, UINT64_MAX };
    const char junk[] = { '/', ':', 'x', ' ', '-', '+', '\0', (char)0xB0 };// '0'-1、'9'+1、字母、空白、符号、NUL、高位字节
    const char *fixed[] = {
        "000000000000000000001", "000000000000000000000", "100000000000000000000",
        "184467440737095516150", "999999999999999999999", "9999999999999999999999999",
        "0000000000000000000000018446744073709551615", "0000000000000000000000018446744073709551616"
    };
    char num[32], buf[64];
    size_t mi, ji, fi, lens, pos, k;
    uint64_t max, start, cnt, c;
    int32_t acc, bad = 0;
    for (mi = 0; mi < ARRAY_SIZE(maxs); mi++) {
        max = maxs[mi];
        start = max >= 12 ? max - 12 : 0;
        cnt = max - start + 13;// start..max+12
        snprintf(num, sizeof(num), "%" PRIu64, start);
        acc = 0;
        for (c = 0; c < cnt; c++, _dec_inc(num)) {
            lens = strlen(num);
            // 原串：只有 <= max 的那几个收下
            if (!_str2u64_same(num, lens, max, &acc)) {
                bad++;
            }
            // lens 只取前缀
            if (lens > 1 && !_str2u64_same(num, lens - 1, max, NULL)) {
                bad++;
            }
            // 前导零
            snprintf(buf, sizeof(buf), "000%s", num);
            if (!_str2u64_same(buf, lens + 3, max, NULL)) {
                bad++;
            }
            // 首、中、尾插一个非数字
            for (ji = 0; ji < sizeof(junk); ji++) {
                for (k = 0; k < 3; k++) {
                    pos = 0 == k ? 0 : (1 == k ? lens / 2 : lens);
                    memcpy(buf, num, pos);
                    buf[pos] = junk[ji];
                    memcpy(buf + pos + 1, num + pos, lens - pos + 1);
                    if (!_str2u64_same(buf, lens + 1, max, NULL)) {
                        bad++;
                    }
                }
            }
        }
        // 收下的个数必须正好是 start..max
        if ((uint64_t)acc != max - start + 1) {
            bad++;
        }
        // 超长串 / 大量前导零
        for (fi = 0; fi < ARRAY_SIZE(fixed); fi++) {
            if (!_str2u64_same(fixed[fi], strlen(fixed[fi]), max, NULL)) {
                bad++;
            }
        }
    }
    CuAssertIntEquals(tc, 0, bad);
}

// ASCII 折叠参考：只折 A-Z
static int32_t _fold_ref(int32_t c) {
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}
// memstr 的朴素参考：逐个起点整段比
static const char *_memstr_ref(int32_t ncs, const char *p, size_t plens, const char *w, size_t wlen) {
    size_t i, j;
    int32_t a, b;
    if (0 == wlen
        || wlen > plens) {
        return NULL;
    }
    for (i = 0; i + wlen <= plens; i++) {
        for (j = 0; j < wlen; j++) {
            a = (unsigned char)p[i + j];
            b = (unsigned char)w[j];
            if (0 != ncs) {
                a = _fold_ref(a);
                b = _fold_ref(b);
            }
            if (a != b) {
                break;
            }
        }
        if (j == wlen) {
            return p + i;
        }
    }
    return NULL;
}
static uint32_t _lcg_next(uint32_t *s) {
    *s = *s * 1103515245u + 12345u;
    return *s >> 16;
}
// 小字母表制造大量部分重叠；源缓冲 plens 之后也填随机字母，越窗读会被比出来
static void test_memstr_ref(CuTest *tc) {
    const char *alpha[2] = { "aAb", "aAbB" };
    char hay[MEMSTR_HAY + 8], what[MEMSTR_HAY + 8];
    uint32_t seed = 20260925u;
    size_t alen, plens, wlen, pos, i;
    int32_t ncs, r, mode, bad = 0, nhit = 0, nmiss = 0, nend = 0;
    const char *got;
    for (ncs = 0; ncs < 2; ncs++) {
        alen = strlen(alpha[ncs]);
        for (r = 0; r < MEMSTR_ROUNDS; r++) {
            for (i = 0; i < sizeof(hay); i++) {
                hay[i] = alpha[ncs][_lcg_next(&seed) % alen];
            }
            plens = 1 + _lcg_next(&seed) % MEMSTR_HAY;
            mode = (int32_t)(_lcg_next(&seed) % 6);
            // 0: wlen==1  1: wlen==plens  2: 源末尾子串  3: 任意子串  4: 随机串  5: wlen 超过 plens
            if (0 == mode) {
                wlen = 1;
            } else if (1 == mode) {
                wlen = plens;
            } else if (5 == mode) {
                wlen = plens + 1 + _lcg_next(&seed) % 4;
            } else {
                wlen = 1 + _lcg_next(&seed) % plens;
            }
            if (mode <= 3 && wlen <= plens) {
                pos = (1 == mode || 2 == mode) ? plens - wlen : _lcg_next(&seed) % (plens - wlen + 1);
                memcpy(what, hay + pos, wlen);
                // ncs=1 时把子串翻成大小写混杂，必须照样命中
                if (0 != ncs) {
                    for (i = 0; i < wlen; i++) {
                        if (0 != (_lcg_next(&seed) & 1) && what[i] >= 'a' && what[i] <= 'z') {
                            what[i] = (char)(what[i] - ('a' - 'A'));
                        }
                    }
                }
            } else {
                for (i = 0; i < wlen; i++) {
                    what[i] = alpha[ncs][_lcg_next(&seed) % alen];
                }
            }
            got = (const char *)memstr(ncs, hay, plens, what, wlen);
            if (got != _memstr_ref(ncs, hay, plens, what, wlen)) {
                bad++;
            }
            if (NULL == got) {
                nmiss++;
            } else {
                nhit++;
                if ((size_t)(got - hay) == plens - wlen) {
                    nend++;
                }
            }
        }
    }
    CuAssertIntEquals(tc, 0, bad);
    CuAssertTrue(tc, nhit > 0 && nmiss > 0 && nend > 0);
    // 确定性边界：匹配在窗口末尾、只差窗口外一个字节、wlen==plens
    CuAssertTrue(tc, NULL == memstr(0, "xxab", 3, "ab", 2));
    CuAssertTrue(tc, NULL == memstr(1, "xxAB", 3, "ab", 2));
    const char *tail = "aaab";
    CuAssertTrue(tc, tail + 2 == memstr(0, tail, 4, "ab", 2));
    CuAssertTrue(tc, tail + 2 == memstr(1, tail, 4, "AB", 2));
    CuAssertTrue(tc, tail + 3 == memstr(0, tail, 4, "b", 1));
    CuAssertTrue(tc, tail == memstr(1, tail, 4, "AAAB", 4));
    CuAssertTrue(tc, NULL == memstr(0, tail, 4, "aaaa", 4));
}

// memichr 的参考：按 (unsigned char)val 折叠后找第一个
static const unsigned char *_memichr_ref(const unsigned char *p, int32_t val, size_t n) {
    int32_t want = _fold_ref((unsigned char)val);
    size_t i;
    for (i = 0; i < n; i++) {
        if (_fold_ref(p[i]) == want) {
            return p + i;
        }
    }
    return NULL;
}
// 倒序全字节表（小写字母排在大写前）上逐 maxlen 比对；val 另覆盖负值与 >255
static void test_memichr_ref(CuTest *tc) {
    unsigned char rev[256];
    int32_t v, bad = 0;
    size_t n;
    for (v = 0; v < 256; v++) {
        rev[v] = (unsigned char)(255 - v);
    }
    for (v = 0; v < 256; v++) {
        for (n = 0; n <= sizeof(rev); n++) {
            if ((const unsigned char *)memichr(rev, v, n) != _memichr_ref(rev, v, n)) {
                bad++;
            }
        }
    }
    for (v = -128; v < 512; v++) {
        if ((const unsigned char *)memichr(rev, v, sizeof(rev)) != _memichr_ref(rev, v, sizeof(rev))) {
            bad++;
        }
    }
    CuAssertIntEquals(tc, 0, bad);
}

// ct_memcmp：任一位置任一位不同都非 0，差异落在 len 之外不算，全等为 0
static void test_ct_memcmp_each_byte(CuTest *tc) {
    unsigned char a[64], b[64];
    size_t len, pos;
    int32_t bit, bad = 0;
    for (pos = 0; pos < sizeof(a); pos++) {
        a[pos] = (unsigned char)(pos * 37 + 11);
    }
    memcpy(b, a, sizeof(a));
    for (len = 0; len <= sizeof(a); len++) {
        if (0 != ct_memcmp(a, b, len)) {
            bad++;
        }
        for (pos = 0; pos < len; pos++) {
            for (bit = 0; bit < 8; bit++) {
                b[pos] ^= (unsigned char)(1u << bit);
                if (0 == ct_memcmp(a, b, len)) {
                    bad++;
                }
                if (0 != ct_memcmp(a, b, pos)) {
                    bad++;
                }
                b[pos] ^= (unsigned char)(1u << bit);
            }
        }
    }
    CuAssertIntEquals(tc, 0, bad);
}

// pack/unpack_float/double 对照 IEEE754 已知字节，缓冲故意不对齐；NaN 载荷与 -0.0 要逐位保住
static void test_pack_float_bytes(CuTest *tc) {
    const float fv[] = { 1.0f, -2.5f, 3.14159265f, -0.0f };
    const unsigned char fbe[4][4] = {
        { 0x3F, 0x80, 0x00, 0x00 }, { 0xC0, 0x20, 0x00, 0x00 }, { 0x40, 0x49, 0x0F, 0xDB }, { 0x80, 0x00, 0x00, 0x00 }
    };
    const double dv[] = { 1.0, -2.5, 3.141592653589793, -0.0 };
    const unsigned char dbe[4][8] = {
        { 0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, { 0xC0, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x40, 0x09, 0x21, 0xFB, 0x54, 0x44, 0x2D, 0x18 }, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
    };
    unsigned char raw[9], le[8];
    size_t i, k;
    int32_t bad = 0;
    float f;
    double d;
    uint32_t fb, fb2;
    uint64_t db, db2;
    for (i = 0; i < 4; i++) {
        for (k = 0; k < 4; k++) {
            le[k] = fbe[i][3 - k];
        }
        pack_float((char *)raw + 1, fv[i], 0);
        if (0 != memcmp(raw + 1, fbe[i], 4)) {
            bad++;
        }
        pack_float((char *)raw + 1, fv[i], 1);
        if (0 != memcmp(raw + 1, le, 4)) {
            bad++;
        }
        // 解包比位模式，-0.0 与 0.0 用 == 分不开
        memcpy(&fb, &fv[i], 4);
        memcpy(raw + 1, fbe[i], 4);
        f = unpack_float((const char *)raw + 1, 0);
        memcpy(&fb2, &f, 4);
        if (fb != fb2) {
            bad++;
        }
        memcpy(raw + 1, le, 4);
        f = unpack_float((const char *)raw + 1, 1);
        memcpy(&fb2, &f, 4);
        if (fb != fb2) {
            bad++;
        }
    }
    for (i = 0; i < 4; i++) {
        for (k = 0; k < 8; k++) {
            le[k] = dbe[i][7 - k];
        }
        pack_double((char *)raw + 1, dv[i], 0);
        if (0 != memcmp(raw + 1, dbe[i], 8)) {
            bad++;
        }
        pack_double((char *)raw + 1, dv[i], 1);
        if (0 != memcmp(raw + 1, le, 8)) {
            bad++;
        }
        memcpy(&db, &dv[i], 8);
        memcpy(raw + 1, dbe[i], 8);
        d = unpack_double((const char *)raw + 1, 0);
        memcpy(&db2, &d, 8);
        if (db != db2) {
            bad++;
        }
        memcpy(raw + 1, le, 8);
        d = unpack_double((const char *)raw + 1, 1);
        memcpy(&db2, &d, 8);
        if (db != db2) {
            bad++;
        }
    }
    // 带载荷的 quiet NaN 大小端往返
    fb = 0x7FC01234u;
    memcpy(&f, &fb, 4);
    pack_float((char *)raw + 1, f, 0);
    f = unpack_float((const char *)raw + 1, 0);
    memcpy(&fb2, &f, 4);
    if (fb != fb2) {
        bad++;
    }
    db = 0x7FF8000000001234ULL;
    memcpy(&d, &db, 8);
    pack_double((char *)raw + 1, d, 1);
    d = unpack_double((const char *)raw + 1, 1);
    memcpy(&db2, &d, 8);
    if (db != db2) {
        bad++;
    }
    CuAssertIntEquals(tc, 0, bad);
#if !defined(OS_WIN) && !defined(OS_DARWIN) && !defined(OS_AIX)
    // 这些平台用 base.c 自己的 ntohll/htonll：每个字节都不对称，换错一段就比得出来
    const uint64_t hv = 0xF1E2D3C4B5A69788ULL;
    const unsigned char hbe[8] = { 0xF1, 0xE2, 0xD3, 0xC4, 0xB5, 0xA6, 0x97, 0x88 };
    uint64_t hn = htonll(hv);
    CuAssertTrue(tc, 0 == memcmp(&hn, hbe, sizeof(hbe)));
    memcpy(&hn, hbe, sizeof(hbe));
    CuAssertTrue(tc, hv == ntohll(hn));
#endif
}

/* sfid_init 失败时一个字节都不写 ctx：校验全走局部量，
 * 调用方忽略返回值也拿不到半初始化的 ctx（timestampshift 是垃圾位移量） */
static void test_sfid_init_keeps_ctx(CuTest *tc) {
    sfid_ctx ctx, probe;
    memset(&ctx, 0x5A, sizeof(ctx));
    memcpy(&probe, &ctx, sizeof(ctx));
    /* 位数总和越界 */
    CuAssertTrue(tc, NULL == sfid_init(&ctx, 0, 12, 12, 0));
    CuAssertTrue(tc, 0 == memcmp(&ctx, &probe, sizeof(ctx)));
    /* 机器ID 越界 */
    CuAssertTrue(tc, NULL == sfid_init(&ctx, 99999, 10, 12, 0));
    CuAssertTrue(tc, 0 == memcmp(&ctx, &probe, sizeof(ctx)));
    /* customepoch 落在未来 */
    CuAssertTrue(tc, NULL == sfid_init(&ctx, 1, 0, 0, nowms() + 3600llu * 1000));
    CuAssertTrue(tc, 0 == memcmp(&ctx, &probe, sizeof(ctx)));
    /* 成功那次才写,且残留的 0x5A 被清干净 */
    CuAssertPtrNotNull(tc, sfid_init(&ctx, 1, 0, 0, 0));
    CuAssertIntEquals(tc, 0, ctx.sequence);
    CuAssertIntEquals(tc, 0, ctx.clockback_warned);
    CuAssertIntEquals(tc, 22, ctx.timestampshift);
}

/* 备用槽：稳态 append+drain 循环只应在首轮走一次分配器。
 * 这条断言是第 1 条优化的唯一硬判据——改之前同样的循环会得到 ROUNDS 次分配 */
static void test_buffer_node_spare(CuTest *tc) {
    char data[64];
    memset(data, 'x', sizeof(data));
    buffer_ctx buf;
    buffer_init(&buf);
    // 先跑一轮把备用槽填上，免得把"首次分配"算进统计窗口
    CuAssertTrue(tc, ERR_OK == buffer_append(&buf, data, sizeof(data)));
    CuAssertTrue(tc, sizeof(data) == buffer_drain(&buf, sizeof(data)));
    const int32_t rounds = 1000;
    uint64_t a0, f0, a1, f1;
    int32_t i;
    mem_stat(&a0, &f0);
    for (i = 0; i < rounds; i++) {
        CuAssertTrue(tc, ERR_OK == buffer_append(&buf, data, sizeof(data)));
        CuAssertTrue(tc, sizeof(data) == buffer_size(&buf));
        CuAssertTrue(tc, sizeof(data) == buffer_drain(&buf, sizeof(data)));
    }
    mem_stat(&a1, &f1);
    CuAssertTrue(tc, 0 == buffer_size(&buf));
    // 全程零分配零释放：节点在备用槽里来回取还
    CuAssertTrue(tc, a1 == a0);
    CuAssertTrue(tc, f1 == f0);
    buffer_free(&buf);
    buffer_thread_cleanup();
}
/* 三条快路径与通用路径的对拍：单节点 search、跨节点 search、buffer_at。
 * 单节点走 memchr 直路，双节点（buffer_external 造出来）走通用路径，两者结果必须一致 */
/* =======================================================================
 * buffer_at 的尾节点直取：落在最后一个有数据节点时按 tail_with_data 一步到位，
 * 不再从 head 逐节点游走。协议层校验包尾 CRLF（redis bulk / http chunk）走的就是这条。
 * 用 external 造多节点链，逐字节与"一次性拼好的单节点"对拍，跨界处也要对
 * ======================================================================= */
static void test_buffer_at_tail_node(CuTest *tc) {
    // 三个节点：12 + 7 + 9 = 28 字节，内容各不相同便于定位错位
    const char *s1 = "AAAABBBBCCCC";
    const char *s2 = "DDDEEEE";
    const char *s3 = "FFFGGGHH";
    size_t l1 = strlen(s1), l2 = strlen(s2), l3 = strlen(s3);
    size_t total = l1 + l2 + l3;
    char *p1, *p2, *p3;
    MALLOC(p1, l1); MALLOC(p2, l2); MALLOC(p3, l3);
    memcpy(p1, s1, l1); memcpy(p2, s2, l2); memcpy(p3, s3, l3);
    buffer_ctx many;
    buffer_init(&many);
    buffer_external(&many, p1, l1, _ext_free);
    buffer_external(&many, p2, l2, _ext_free);
    buffer_external(&many, p3, l3, _ext_free);
    CuAssertTrue(tc, total == buffer_size(&many));
    // 单节点参照
    char whole[64];
    memcpy(whole, s1, l1); memcpy(whole + l1, s2, l2); memcpy(whole + l1 + l2, s3, l3);
    buffer_ctx one;
    buffer_init(&one);
    CuAssertTrue(tc, ERR_OK == buffer_append(&one, whole, total));
    // 逐字节对拍：覆盖首节点、中间节点、尾节点以及两处跨界
    size_t i;
    for (i = 0; i < total; i++) {
        CuAssertIntEquals(tc, whole[i], buffer_at(&many, i));
        CuAssertIntEquals(tc, whole[i], buffer_at(&one, i));
    }
    // 末两字节是本用例的正主（协议层校验包尾 CRLF 的形状）
    CuAssertIntEquals(tc, whole[total - 2], buffer_at(&many, total - 2));
    CuAssertIntEquals(tc, whole[total - 1], buffer_at(&many, total - 1));
    // 倒序再走一遍：不依赖 hint 被前一次调用推到尾部
    for (i = total; i > 0; i--) {
        CuAssertIntEquals(tc, whole[i - 1], buffer_at(&many, i - 1));
    }
    // drain 掉首节点后尾节点的基偏移随之变化，直取必须跟着对
    CuAssertTrue(tc, l1 == buffer_drain(&many, l1));
    CuAssertTrue(tc, (total - l1) == buffer_size(&many));
    for (i = 0; i < total - l1; i++) {
        CuAssertIntEquals(tc, whole[l1 + i], buffer_at(&many, i));
    }
    buffer_free(&many);
    buffer_free(&one);
}
static void test_buffer_fastpath_equiv(CuTest *tc) {
    const char *body = "GET /path HTTP/1.1\r\nHost: a.b\r\n\r\nBODY";
    size_t blens = strlen(body);
    buffer_ctx one;
    buffer_init(&one);
    CuAssertTrue(tc, ERR_OK == buffer_append(&one, (void *)body, blens));
    // 同样内容切成两个 external 节点，强制走通用路径
    char *p1, *p2;
    size_t cut = 10;
    MALLOC(p1, cut);
    MALLOC(p2, blens - cut);
    memcpy(p1, body, cut);
    memcpy(p2, body + cut, blens - cut);
    buffer_ctx two;
    buffer_init(&two);
    buffer_external(&two, p1, cut, _ext_free);
    buffer_external(&two, p2, blens - cut, _ext_free);
    CuAssertTrue(tc, blens == buffer_size(&one));
    CuAssertTrue(tc, blens == buffer_size(&two));
    const char *pats[] = { "\r\n\r\n", "\r\n", "H", "HTTP/1.1", "Host: a.b", "zz", "BODY", "Y" };
    size_t i;
    int32_t r1, r2;
    for (i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
        r1 = buffer_search(&one, 0, 0, 0, (char *)pats[i], strlen(pats[i]));
        r2 = buffer_search(&two, 0, 0, 0, (char *)pats[i], strlen(pats[i]));
        CuAssertIntEquals(tc, r2, r1);
    }
    // 带 start/end 窗口
    for (i = 0; i < blens; i++) {
        r1 = buffer_search(&one, 0, i, 0, "\r\n", 2);
        r2 = buffer_search(&two, 0, i, 0, "\r\n", 2);
        CuAssertIntEquals(tc, r2, r1);
    }
    // buffer_at 逐字节对拍
    for (i = 0; i < blens; i++) {
        CuAssertTrue(tc, buffer_at(&one, i) == buffer_at(&two, i));
        CuAssertTrue(tc, buffer_at(&one, i) == body[i]);
    }
    // drain 一半后再对拍一轮：单节点走的是 misalign 前移那条直路
    CuAssertTrue(tc, 5 == buffer_drain(&one, 5));
    CuAssertTrue(tc, 5 == buffer_drain(&two, 5));
    for (i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
        r1 = buffer_search(&one, 0, 0, 0, (char *)pats[i], strlen(pats[i]));
        r2 = buffer_search(&two, 0, 0, 0, (char *)pats[i], strlen(pats[i]));
        CuAssertIntEquals(tc, r2, r1);
    }
    for (i = 0; i < blens - 5; i++) {
        CuAssertTrue(tc, buffer_at(&one, i) == buffer_at(&two, i));
    }
    buffer_free(&one);
    buffer_free(&two);
}

/* =======================================================================
 * mmap —— 文件 / 匿名 / 命名共享内存映射
 * ======================================================================= */
// 临时文件放在可执行文件目录下
static void _mm_path(char *buf, size_t lens, const char *name) {
    SNPRINTF(buf, lens, "%s%s%s", procpath(), PATH_SEPARATORSTR, name);
}
// 写一个第 i 字节为 i % 251 的文件
static int32_t _mm_mkfile(const char *path, size_t lens) {
    FILE *fp = fopen_cloexec(path, "wb");
    size_t i;
    char c;
    if (NULL == fp) {
        return ERR_FAILED;
    }
    for (i = 0; i < lens; i++) {
        c = (char)(i % 251);
        fwrite(&c, 1, 1, fp);
    }
    fclose(fp);
    return ERR_OK;
}
static int32_t _mm_pattern_ok(const char *p, uint64_t foff, size_t lens) {
    size_t i;
    for (i = 0; i < lens; i++) {
        if ((char)((foff + i) % 251) != p[i]) {
            return 0;
        }
    }
    return 1;
}
// 读写往返：新建、写满、刷盘、关掉后只读打开读回；EXCL 撞已存在文件失败
static void test_mmap_rdwr_roundtrip(CuTest *tc) {
    char path[PATH_LENS];
    mmap_ctx mm;
    mmap_opts o;
    size_t i, n = 3 * mmap_granularity() + 123;
    int32_t ok;
    _mm_path(path, sizeof(path), "test_mmap_rw.tmp");
    remove(path);
    ZERO(&o, sizeof(o));
    o.mode = MMAP_RDWR;
    o.flags = MMAP_CREATE | MMAP_EXCL;
    o.size = n;
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    CuAssertTrue(tc, NULL != mm.addr && n == mm.size);
    for (i = 0; i < n; i++) {
        mm.addr[i] = (char)(i % 251);
    }
    CuAssertIntEquals(tc, ERR_OK, mmap_sync(&mm, 0, 0, 0));
    CuAssertTrue(tc, (int64_t)n == mmap_filesize(&mm));
    mmap_close(&mm);
    CuAssertTrue(tc, (int64_t)n == filesize(path));
    ZERO(&o, sizeof(o));
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    ok = n == mm.size && _mm_pattern_ok(mm.addr, 0, n);
    mmap_close(&mm);
    CuAssertTrue(tc, ok);
    o.mode = MMAP_RDWR;
    o.flags = MMAP_CREATE | MMAP_EXCL;
    CuAssertIntEquals(tc, ERR_FAILED, mmap_open(&mm, path, &o));
    CuAssertTrue(tc, NULL == mm.addr);
    mmap_close(&mm);
    remove(path);
}
// 空文件与越界参数：size 为 0 不建映射；只读越过文件末尾、只读预留、非法 mode 都失败；全零 ctx 与重复 close 安全
static void test_mmap_empty_bounds(CuTest *tc) {
    char path[PATH_LENS];
    mmap_ctx mm;
    mmap_opts o;
    size_t n = 1000;
    ZERO(&mm, sizeof(mm));
    mmap_close(&mm);
    _mm_path(path, sizeof(path), "test_mmap_empty.tmp");
    remove(path);
    ZERO(&o, sizeof(o));
    o.mode = MMAP_RDWR;
    o.flags = MMAP_CREATE;
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    CuAssertTrue(tc, NULL == mm.addr && 0 == mm.size);
    mmap_close(&mm);
    mmap_close(&mm);
    CuAssertTrue(tc, 0 == filesize(path));
    ZERO(&o, sizeof(o));
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    CuAssertTrue(tc, NULL == mm.addr && 0 == mm.size);
    mmap_close(&mm);
    CuAssertIntEquals(tc, ERR_OK, _mm_mkfile(path, n));
    o.size = n + 1;
    CuAssertIntEquals(tc, ERR_FAILED, mmap_open(&mm, path, &o));
    o.size = 0;
    o.off = n + 1;
    CuAssertIntEquals(tc, ERR_FAILED, mmap_open(&mm, path, &o));
    o.off = n;
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    CuAssertTrue(tc, NULL == mm.addr && 0 == mm.size);
    mmap_close(&mm);
    o.off = 0;
    o.size = 10;
    o.cap = 100;
    CuAssertIntEquals(tc, ERR_FAILED, mmap_open(&mm, path, &o));
    o.mode = MMAP_COPY;
    CuAssertIntEquals(tc, ERR_FAILED, mmap_open(&mm, path, &o));
    o.mode = 7;
    o.cap = 0;
    CuAssertIntEquals(tc, ERR_FAILED, mmap_open(&mm, path, &o));
    remove(path);
    ZERO(&o, sizeof(o));
    CuAssertIntEquals(tc, ERR_FAILED, mmap_open(&mm, path, &o));
    CuAssertIntEquals(tc, ERR_FAILED, mmap_open(&mm, path, NULL));
}
// 任意偏移：内部向下对齐，addr 已加回偏差
static void test_mmap_offset(CuTest *tc) {
    char path[PATH_LENS];
    mmap_ctx mm;
    mmap_opts o;
    size_t gran = mmap_granularity(), n = 3 * gran + 500;
    int32_t ok;
    _mm_path(path, sizeof(path), "test_mmap_off.tmp");
    CuAssertIntEquals(tc, ERR_OK, _mm_mkfile(path, n));
    ZERO(&o, sizeof(o));
    o.off = gran + 7;
    o.size = 1000;
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    ok = 1000 == mm.size && 7 == mm.delta && _mm_pattern_ok(mm.addr, gran + 7, 1000);
    mmap_close(&mm);
    CuAssertTrue(tc, ok);
    o.mode = MMAP_COPY;
    o.off = 5;
    o.size = 0;
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    ok = n - 5 == mm.size && _mm_pattern_ok(mm.addr, 5, n - 5);
    mmap_close(&mm);
    CuAssertTrue(tc, ok);
    remove(path);
}
// 预留容量：cap 内变长变短 addr 不变；close 时文件截回 off + size
static void test_mmap_cap_grow(CuTest *tc) {
    char path[PATH_LENS];
    mmap_ctx mm;
    mmap_opts o;
    size_t gran = mmap_granularity();
    char *a0;
    int32_t ok;
    _mm_path(path, sizeof(path), "test_mmap_cap.tmp");
    remove(path);
    ZERO(&o, sizeof(o));
    o.mode = MMAP_RDWR;
    o.flags = MMAP_CREATE;
    o.size = 100;
    o.cap = 16 * gran;
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    a0 = mm.addr;
    CuAssertTrue(tc, NULL != a0 && 100 == mm.size && 16 * gran == mm.cap);
    CuAssertTrue(tc, mmap_filesize(&mm) >= 100);
    a0[0] = 'A';
    a0[99] = 'B';
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&mm, 8 * gran + 3, 0));
    CuAssertTrue(tc, a0 == mm.addr && 16 * gran == mm.cap);
    CuAssertTrue(tc, mmap_filesize(&mm) >= (int64_t)(8 * gran + 3));
    a0[8 * gran + 2] = 'C';
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&mm, 2 * gran, 0));
    CuAssertTrue(tc, a0 == mm.addr && 2 * gran == mm.size);
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&mm, 5 * gran, 0));
    CuAssertTrue(tc, a0 == mm.addr);
    a0[5 * gran - 1] = 'D';
    CuAssertIntEquals(tc, ERR_OK, mmap_sync(&mm, 0, 0, 0));
    mmap_close(&mm);
    CuAssertTrue(tc, (int64_t)(5 * gran) == filesize(path));
    ZERO(&o, sizeof(o));
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    ok = 5 * gran == mm.size && 'A' == mm.addr[0] && 'B' == mm.addr[99] && 'D' == mm.addr[5 * gran - 1];
    mmap_close(&mm);
    CuAssertTrue(tc, ok);
    remove(path);
}
// 窗口映射：只截本映射扩出来的部分，映射前已有的数据不动
static void test_mmap_window_keep(CuTest *tc) {
    char path[PATH_LENS];
    mmap_ctx mm;
    mmap_opts o;
    size_t gran = mmap_granularity(), n = 4 * gran;
    int32_t ok;
    _mm_path(path, sizeof(path), "test_mmap_win.tmp");
    CuAssertIntEquals(tc, ERR_OK, _mm_mkfile(path, n));
    ZERO(&o, sizeof(o));
    o.mode = MMAP_RDWR;
    o.off = gran;
    o.size = gran;
    o.cap = 4 * gran;
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&mm, 3 * gran, 0));
    mm.addr[3 * gran - 1] = 'Z';
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&mm, gran / 2, 0));
    mmap_close(&mm);
    CuAssertTrue(tc, (int64_t)n == filesize(path));
    ZERO(&o, sizeof(o));
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    ok = n == mm.size && _mm_pattern_ok(mm.addr, 0, n - 1) && 'Z' == mm.addr[n - 1];
    mmap_close(&mm);
    CuAssertTrue(tc, ok);
    remove(path);
}
// close 只截本映射自己扩出来的部分：别人先写长的那段（含数据）留着
static void test_mmap_trim_others(CuTest *tc) {
    char path[PATH_LENS];
    mmap_ctx a, b;
    mmap_opts o;
    size_t gran = mmap_granularity();
    int32_t ok;
    _mm_path(path, sizeof(path), "test_mmap_trim.tmp");
    remove(path);
    ZERO(&o, sizeof(o));
    o.mode = MMAP_RDWR;
    o.flags = MMAP_CREATE;
    o.size = gran;
    o.cap = 16 * gran;
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&a, path, &o));
    ZERO(&o, sizeof(o));
    o.mode = MMAP_RDWR;
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&b, path, &o));
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&b, 4 * gran, 0));
    b.addr[3 * gran] = 'B';
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&a, 8 * gran, 0));
    a.addr[7 * gran] = 'A';
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&a, gran / 2, 0));
    mmap_close(&b);
    mmap_close(&a);
    CuAssertTrue(tc, (int64_t)(4 * gran) == filesize(path));
    ZERO(&o, sizeof(o));
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&a, path, &o));
    ok = 4 * gran == a.size && 'B' == a.addr[3 * gran];
    mmap_close(&a);
    CuAssertTrue(tc, ok);
    remove(path);
}
// 超出 cap：按新 cap 重映射，内容不丢；之后在新 cap 内 addr 不变
static void test_mmap_beyond_cap(CuTest *tc) {
    char path[PATH_LENS];
    mmap_ctx mm;
    mmap_opts o;
    size_t gran = mmap_granularity();
    char *a1;
    int32_t ok;
    _mm_path(path, sizeof(path), "test_mmap_regrow.tmp");
    remove(path);
    ZERO(&o, sizeof(o));
    o.mode = MMAP_RDWR;
    o.flags = MMAP_CREATE;
    o.size = gran;
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    CuAssertTrue(tc, gran == mm.cap);
    mm.addr[0] = 'H';
    mm.addr[gran - 1] = 'T';
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&mm, 4 * gran + 10, 0));
    CuAssertTrue(tc, 4 * gran + 10 == mm.size && 4 * gran + 10 == mm.cap);
    CuAssertTrue(tc, 'H' == mm.addr[0] && 'T' == mm.addr[gran - 1]);
    mm.addr[4 * gran + 9] = 'E';
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&mm, 4 * gran + 10, 16 * gran));
    CuAssertTrue(tc, 16 * gran == mm.cap && 'H' == mm.addr[0] && 'E' == mm.addr[4 * gran + 9]);
    a1 = mm.addr;
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&mm, 10 * gran, 0));
    CuAssertTrue(tc, a1 == mm.addr);
    mm.addr[10 * gran - 1] = 'F';
    mmap_close(&mm);
    CuAssertTrue(tc, (int64_t)(10 * gran) == filesize(path));
    ZERO(&o, sizeof(o));
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    ok = 'H' == mm.addr[0] && 'T' == mm.addr[gran - 1] && 'E' == mm.addr[4 * gran + 9] && 'F' == mm.addr[10 * gran - 1];
    mmap_close(&mm);
    CuAssertTrue(tc, ok);
    remove(path);
}
// 只读 / 写时复制映射跟上别人写长的文件；写时复制的私有改动重映射后仍在；越过文件末尾或要求预留都失败
static void test_mmap_rdonly_follow(CuTest *tc) {
    char path[PATH_LENS];
    mmap_ctx w, r, c;
    mmap_opts o;
    size_t gran = mmap_granularity();
    _mm_path(path, sizeof(path), "test_mmap_follow.tmp");
    remove(path);
    ZERO(&o, sizeof(o));
    o.mode = MMAP_RDWR;
    o.flags = MMAP_CREATE;
    o.size = gran;
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&w, path, &o));
    w.addr[1] = 'w';
    ZERO(&o, sizeof(o));
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&r, path, &o));
    CuAssertTrue(tc, gran == r.size && 'w' == r.addr[1]);
    o.mode = MMAP_COPY;
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&c, path, &o));
    c.addr[1] = 'q';
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&w, 3 * gran, 0));
    w.addr[2 * gran + 5] = 'n';
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&c, 3 * gran, 0));
    CuAssertTrue(tc, 'q' == c.addr[1] && 'n' == c.addr[2 * gran + 5] && 'w' == w.addr[1]);
    mmap_close(&c);
    CuAssertTrue(tc, (int64_t)(3 * gran) == mmap_filesize(&r));
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&r, 3 * gran, 0));
    CuAssertTrue(tc, 'n' == r.addr[2 * gran + 5] && 'w' == r.addr[1]);
    CuAssertIntEquals(tc, ERR_FAILED, mmap_resize(&r, 3 * gran + 1, 0));
    CuAssertIntEquals(tc, ERR_FAILED, mmap_resize(&r, gran, 2 * gran));
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&r, gran, 0));
    CuAssertTrue(tc, 'w' == r.addr[1]);
    mmap_close(&r);
    mmap_close(&w);
    remove(path);
}
// 写时复制与保护属性：改动不进文件；只读映射不能改成可写
static void test_mmap_copy_protect(CuTest *tc) {
    char path[PATH_LENS];
    mmap_ctx mm;
    mmap_opts o;
    size_t gran = mmap_granularity(), n = 2 * gran;
    int32_t ok;
    _mm_path(path, sizeof(path), "test_mmap_cow.tmp");
    CuAssertIntEquals(tc, ERR_OK, _mm_mkfile(path, n));
    ZERO(&o, sizeof(o));
    o.mode = MMAP_COPY;
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    mm.addr[0] = 'Z';
    CuAssertIntEquals(tc, ERR_OK, mmap_protect(&mm, 0, gran, MMAP_RDONLY));
    CuAssertIntEquals(tc, ERR_OK, mmap_protect(&mm, 0, gran, MMAP_RDWR));
    mm.addr[1] = 'Y';
    CuAssertIntEquals(tc, ERR_FAILED, mmap_protect(&mm, 0, 0, MMAP_COPY));
    CuAssertIntEquals(tc, ERR_FAILED, mmap_protect(&mm, n, 1, MMAP_RDONLY));
    CuAssertIntEquals(tc, ERR_OK, mmap_sync(&mm, 0, 0, 0));
    mmap_close(&mm);
    ZERO(&o, sizeof(o));
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    ok = _mm_pattern_ok(mm.addr, 0, n);
    CuAssertIntEquals(tc, ERR_FAILED, mmap_protect(&mm, 0, 0, MMAP_RDWR));
    CuAssertIntEquals(tc, ERR_OK, mmap_protect(&mm, 0, 0, MMAP_RDONLY));
    mmap_close(&mm);
    CuAssertTrue(tc, ok);
    o.mode = MMAP_RDWR;
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    CuAssertIntEquals(tc, ERR_OK, mmap_protect(&mm, 10, 100, MMAP_RDONLY));
    CuAssertIntEquals(tc, ERR_OK, mmap_protect(&mm, 10, 100, MMAP_RDWR));
    mm.addr[20] = 'P';
    mmap_close(&mm);
    ZERO(&o, sizeof(o));
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    ok = 'P' == mm.addr[20];
    mmap_close(&mm);
    CuAssertTrue(tc, ok);
    remove(path);
}
// 按句柄映射：内部复制句柄，调用方关掉自己的之后刷盘、变长照样可用
static void test_mmap_map_fd(CuTest *tc) {
    char path[PATH_LENS];
    mmap_ctx mm;
    mmap_opts o;
    size_t n = 5000;
    mmap_fd fd;
    int32_t ok;
    _mm_path(path, sizeof(path), "test_mmap_fd.tmp");
    CuAssertIntEquals(tc, ERR_OK, _mm_mkfile(path, n));
#ifdef OS_WIN
    fd = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CuAssertTrue(tc, INVALID_HANDLE_VALUE != fd);
#else
    fd = open(path, O_RDWR | O_CLOEXEC);
    CuAssertTrue(tc, -1 != fd);
#endif
    ZERO(&o, sizeof(o));
    o.mode = MMAP_RDWR;
    ok = ERR_OK == mmap_map_fd(&mm, fd, &o);
#ifdef OS_WIN
    CloseHandle(fd);
#else
    close(fd);
#endif
    CuAssertTrue(tc, ok);
    CuAssertTrue(tc, n == mm.size && _mm_pattern_ok(mm.addr, 0, n));
    mm.addr[0] = 'F';
    CuAssertIntEquals(tc, ERR_OK, mmap_sync(&mm, 0, 0, 0));
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&mm, 2 * n, 0));
    mm.addr[2 * n - 1] = 'L';
    mmap_close(&mm);
    CuAssertTrue(tc, (int64_t)(2 * n) == filesize(path));
    ZERO(&o, sizeof(o));
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    ok = 'F' == mm.addr[0] && 'L' == mm.addr[2 * n - 1];
    mmap_close(&mm);
    CuAssertTrue(tc, ok);
#ifdef OS_WIN
    CuAssertIntEquals(tc, ERR_FAILED, mmap_map_fd(&mm, INVALID_HANDLE_VALUE, &o));
#else
    CuAssertIntEquals(tc, ERR_FAILED, mmap_map_fd(&mm, -1, &o));
#endif
    remove(path);
}
// 匿名映射：初始为 0；cap 内变长变短 addr 不变，还回去再提交的部分重新为 0；超出 cap 拷过去
static void test_mmap_anon(CuTest *tc) {
    mmap_ctx mm;
    mmap_opts o;
    size_t i, gran = mmap_granularity();
    char *a0;
    int32_t ok = 1;
    ZERO(&o, sizeof(o));
    o.size = 3 * gran;
    o.cap = 64 * gran;
    CuAssertIntEquals(tc, ERR_OK, mmap_anon(&mm, &o));
    a0 = mm.addr;
    for (i = 0; i < 3 * gran; i++) {
        ok = ok && 0 == a0[i];
    }
    CuAssertTrue(tc, ok);
    memset(a0, 0x5a, 3 * gran);
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&mm, 10 * gran, 0));
    CuAssertTrue(tc, a0 == mm.addr && 0x5a == a0[3 * gran - 1] && 0 == a0[10 * gran - 1]);
    memset(a0, 0x5a, 10 * gran);
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&mm, gran, 0));
    CuAssertTrue(tc, a0 == mm.addr && 0x5a == a0[gran - 1]);
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&mm, 4 * gran, 0));
    CuAssertTrue(tc, a0 == mm.addr && 0x5a == a0[0] && 0 == a0[2 * gran] && 0 == a0[4 * gran - 1]);
    CuAssertTrue(tc, ERR_FAILED == mmap_filesize(&mm));
    CuAssertIntEquals(tc, ERR_OK, mmap_sync(&mm, 0, 0, 0));
    CuAssertIntEquals(tc, ERR_FAILED, mmap_advise(&mm, 0, 0, MMAP_DONTNEED));
    CuAssertIntEquals(tc, ERR_OK, mmap_advise(&mm, 0, 0, MMAP_WILLNEED));
    CuAssertIntEquals(tc, ERR_OK, mmap_lock(&mm, 0, gran));
    CuAssertIntEquals(tc, ERR_OK, mmap_unlock(&mm, 0, gran));
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&mm, 100 * gran, 0));
    CuAssertTrue(tc, 100 * gran == mm.cap && 0x5a == mm.addr[0] && 0x5a == mm.addr[gran - 1] && 0 == mm.addr[100 * gran - 1]);
    mmap_close(&mm);
    o.size = 0;
    o.cap = 8 * gran;
    CuAssertIntEquals(tc, ERR_OK, mmap_anon(&mm, &o));
    CuAssertTrue(tc, NULL == mm.addr);
    CuAssertTrue(tc, mmap_in_range(&mm, 0, 0) && !mmap_in_range(&mm, 0, 1));
    CuAssertTrue(tc, NULL == mmap_ptr(&mm, 0, 0) && 0 == mmap_size(&mm));
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&mm, gran, 0));
    a0 = mm.addr;
    CuAssertTrue(tc, NULL != a0 && 8 * gran == mm.cap);
    CuAssertIntEquals(tc, ERR_OK, mmap_resize(&mm, 8 * gran, 0));
    CuAssertTrue(tc, a0 == mm.addr);
    mmap_close(&mm);
}
// 命名共享内存：创建、另一个视图看得见、EXCL 冲突、不支持 resize；生命周期按平台区分
static void test_mmap_shm(CuTest *tc) {
    char name[32], bad[40];
    mmap_ctx a, b, c;
    mmap_opts o;
    SNPRINTF(name, sizeof(name), "/srey_t_%d", (int32_t)GETPID());
    (void)mmap_shm_unlink(name);
    ZERO(&o, sizeof(o));
    o.mode = MMAP_RDWR;
    o.flags = MMAP_CREATE | MMAP_EXCL;
    o.size = 5000;
    CuAssertIntEquals(tc, ERR_OK, mmap_shm(&a, name, &o));
    CuAssertTrue(tc, 5000 == a.size);
    memcpy(a.addr, "hello", 5);
    a.addr[4999] = 'E';
    CuAssertIntEquals(tc, ERR_FAILED, mmap_shm(&c, name, &o));
    ZERO(&o, sizeof(o));
    CuAssertIntEquals(tc, ERR_OK, mmap_shm(&b, name, &o));
    CuAssertTrue(tc, b.size >= 5000 && 0 == memcmp(b.addr, "hello", 5) && 'E' == b.addr[4999]);
    o.mode = MMAP_COPY;
    CuAssertIntEquals(tc, ERR_FAILED, mmap_shm(&c, name, &o));
    o.mode = MMAP_RDONLY;
    o.size = 1024 * 1024;
    CuAssertIntEquals(tc, ERR_FAILED, mmap_shm(&c, name, &o));
    CuAssertIntEquals(tc, ERR_FAILED, mmap_resize(&a, 100, 0));
    CuAssertTrue(tc, ERR_FAILED == mmap_filesize(&a));
    CuAssertIntEquals(tc, ERR_OK, mmap_sync(&a, 0, 0, 0));
    mmap_close(&b);
    mmap_close(&a);
    o.size = 0;
#ifdef OS_WIN
    CuAssertIntEquals(tc, ERR_FAILED, mmap_shm(&c, name, &o));
#else
    CuAssertIntEquals(tc, ERR_OK, mmap_shm(&c, name, &o));
    int32_t ok = 0 == memcmp(c.addr, "hello", 5);
    mmap_close(&c);
    CuAssertTrue(tc, ok);
    CuAssertIntEquals(tc, ERR_OK, mmap_shm_unlink(name));
    CuAssertIntEquals(tc, ERR_FAILED, mmap_shm(&c, name, &o));
#endif
    CuAssertIntEquals(tc, ERR_FAILED, mmap_shm(&c, "noslash", &o));
    CuAssertIntEquals(tc, ERR_FAILED, mmap_shm(&c, "/a/b", &o));
    CuAssertIntEquals(tc, ERR_FAILED, mmap_shm(&c, "/", &o));
    memset(bad, 'x', sizeof(bad));
    bad[0] = '/';
    bad[32] = '\0';
    CuAssertIntEquals(tc, ERR_FAILED, mmap_shm(&c, bad, &o));
    CuAssertIntEquals(tc, ERR_FAILED, mmap_shm_unlink(bad));
    o.flags = MMAP_CREATE;
    CuAssertIntEquals(tc, ERR_FAILED, mmap_shm(&c, name, &o));
}
// advise / sync / populate 的参数与越界检查
static void test_mmap_advise_sync(CuTest *tc) {
    char path[PATH_LENS];
    mmap_ctx mm;
    mmap_opts o;
    size_t gran = mmap_granularity(), n = 4 * gran;
    int32_t ok;
    _mm_path(path, sizeof(path), "test_mmap_adv.tmp");
    CuAssertIntEquals(tc, ERR_OK, _mm_mkfile(path, n));
    ZERO(&o, sizeof(o));
    o.mode = MMAP_RDWR;
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    CuAssertIntEquals(tc, ERR_OK, mmap_advise(&mm, 0, 0, MMAP_NORMAL));
    CuAssertIntEquals(tc, ERR_OK, mmap_advise(&mm, 1, 100, MMAP_SEQUENTIAL));
    CuAssertIntEquals(tc, ERR_OK, mmap_advise(&mm, gran + 3, gran, MMAP_RANDOM));
    CuAssertIntEquals(tc, ERR_OK, mmap_advise(&mm, 0, 0, MMAP_WILLNEED));
    mm.addr[7] = 'Q';
    CuAssertIntEquals(tc, ERR_OK, mmap_advise(&mm, 0, 0, MMAP_DONTNEED));
    CuAssertTrue(tc, 'Q' == mm.addr[7] && _mm_pattern_ok(mm.addr + 8, 8, n - 8));
    CuAssertIntEquals(tc, ERR_FAILED, mmap_advise(&mm, 0, 0, 9));
    CuAssertIntEquals(tc, ERR_FAILED, mmap_advise(&mm, n + 1, 0, MMAP_NORMAL));
    CuAssertIntEquals(tc, ERR_FAILED, mmap_advise(&mm, n - 1, 2, MMAP_NORMAL));
    CuAssertIntEquals(tc, ERR_OK, mmap_advise(&mm, n, 0, MMAP_NORMAL));
    // 越界宏：恰到末尾合法，越过一个字节或 off + lens 溢出都判越界
    CuAssertTrue(tc, mmap_in_range(&mm, 0, n) && mmap_in_range(&mm, n, 0) && mmap_in_range(&mm, n - 1, 1));
    CuAssertTrue(tc, !mmap_in_range(&mm, n, 1) && !mmap_in_range(&mm, n + 1, 0) && !mmap_in_range(&mm, 1, SIZE_MAX));
    // 取地址：范围内返回 addr + off，越界返回 NULL
    CuAssertTrue(tc, n == mmap_size(&mm) && mm.addr == mmap_ptr(&mm, 0, n) && mm.addr + 5 == mmap_ptr(&mm, 5, 10));
    CuAssertTrue(tc, NULL == mmap_ptr(&mm, n, 1) && NULL == mmap_ptr(&mm, 1, SIZE_MAX));
    CuAssertIntEquals(tc, ERR_OK, mmap_sync(&mm, 5, 100, 1));
    CuAssertIntEquals(tc, ERR_OK, mmap_sync(&mm, gran + 1, 10, 0));
    CuAssertIntEquals(tc, ERR_FAILED, mmap_sync(&mm, n, 1, 0));
    mmap_close(&mm);
    CuAssertIntEquals(tc, ERR_FAILED, mmap_sync(&mm, 0, 0, 0));
    ZERO(&o, sizeof(o));
    o.flags = MMAP_POPULATE;
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    ok = 'Q' == mm.addr[7] && _mm_pattern_ok(mm.addr + 8, 8, n - 8);
    CuAssertIntEquals(tc, ERR_OK, mmap_sync(&mm, 0, 0, 0));
    mmap_close(&mm);
    CuAssertTrue(tc, ok);
    o.mode = MMAP_COPY;
    o.flags = 0;
    CuAssertIntEquals(tc, ERR_OK, mmap_open(&mm, path, &o));
    CuAssertIntEquals(tc, ERR_FAILED, mmap_advise(&mm, 0, 0, MMAP_DONTNEED));
    mmap_close(&mm);
    remove(path);
}
static void test_mmap_granularity(CuTest *tc) {
    size_t gran = mmap_granularity();
    CuAssertTrue(tc, gran >= 4096 && 0 == (gran & (gran - 1)));
}

/* =======================================================================
 * 效率改写的等价性：输出必须与改写前逐字节相同
 * ======================================================================= */
// netaddr_ip 的 IPv4 手写转换对照 inet_ntop，覆盖 1/2/3 位数各档与边界
static void test_netaddr_ip4_format(CuTest *tc) {
    static const uint8_t vals[] = { 0, 1, 9, 10, 11, 99, 100, 101, 199, 200, 249, 250, 255 };
    const size_t n = sizeof(vals);
    netaddr_ctx addr;
    char ip[IP_LENS], ref[IP_LENS];
    uint8_t b[4];
    size_t i, j, k;
    ZERO(&addr, sizeof(addr));
    addr.ipv4.sin_family = AF_INET;
    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++) {
            for (k = 0; k < n; k++) {
                b[0] = vals[i];
                b[1] = vals[j];
                b[2] = vals[k];
                b[3] = vals[(i + j + k) % n];
                memcpy(&addr.ipv4.sin_addr, b, sizeof(b));
                CuAssertIntEquals(tc, ERR_OK, netaddr_ip(&addr, ip));
                CuAssertPtrNotNull(tc, inet_ntop(AF_INET, &addr.ipv4.sin_addr, ref, sizeof(ref)));
                CuAssertStrEquals(tc, ref, ip);
            }
        }
    }
    // 0~255 每个值在每一段都出现一次(查表逐项)
    for (i = 0; i < 256; i++) {
        b[0] = (uint8_t)i;
        b[1] = (uint8_t)(255 - i);
        b[2] = (uint8_t)(i * 7);
        b[3] = (uint8_t)(i * 13 + 5);
        memcpy(&addr.ipv4.sin_addr, b, sizeof(b));
        CuAssertIntEquals(tc, ERR_OK, netaddr_ip(&addr, ip));
        CuAssertPtrNotNull(tc, inet_ntop(AF_INET, &addr.ipv4.sin_addr, ref, sizeof(ref)));
        CuAssertStrEquals(tc, ref, ip);
    }
}
// Sakamoto 算法独立算星期(0 = 周日)
static int32_t _dow_ref(int32_t y, int32_t m, int32_t d) {
    static const int32_t t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) {
        y -= 1;
    }
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
// _strptime 由年月日补出的 tm_wday：逐日覆盖 1896~2104(含闰年、世纪年、400 年闰)
static void test_strptime_wday(CuTest *tc) {
    static const int32_t mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    char buf[32];
    struct tm tm;
    int32_t y, m, d, dim, leap;
    for (y = 1896; y <= 2104; y++) {
        leap = (0 == y % 4 && 0 != y % 100) || 0 == y % 400;
        for (m = 1; m <= 12; m++) {
            dim = mdays[m - 1] + ((2 == m && leap) ? 1 : 0);
            for (d = 1; d <= dim; d++) {
                snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
                ZERO(&tm, sizeof(tm));
                CuAssertPtrNotNull(tc, _strptime(buf, "%Y-%m-%d", &tm));
                CuAssertIntEquals(tc, _dow_ref(y, m, d), tm.tm_wday);
            }
        }
    }
}
// hash_str 与 hash(s, strlen(s)) 结果相同(含空串与高位字节)
static void test_hash_str(CuTest *tc) {
    static const char *strs[] = { "", "a", "harbor", "task_name_with_a_longer_suffix_0123456789", "\xff\x80z" };
    size_t i;
    for (i = 0; i < ARRAY_SIZE(strs); i++) {
        CuAssertTrue(tc, hash_str(strs[i]) == hash(strs[i], strlen(strs[i])));
    }
}
// binary_set_uint 对照 snprintf 的 %llu / %llx
static void test_binary_set_uint(CuTest *tc) {
    static const uint64_t vals[] = { 0, 1, 9, 10, 15, 16, 255, 4096, 1000000007ULL, UINT64_MAX };
    binary_ctx bw;
    char ref[80];
    size_t i;
    for (i = 0; i < ARRAY_SIZE(vals); i++) {
        binary_init_write(&bw, 0, 0);
        binary_set_uint(&bw, vals[i], 10);
        snprintf(ref, sizeof(ref), "%"PRIu64, vals[i]);
        CuAssertTrue(tc, strlen(ref) == bw.offset && 0 == memcmp(ref, bw.data, bw.offset));
        binary_offset(&bw, 0);
        binary_set_uint(&bw, vals[i], 16);
        snprintf(ref, sizeof(ref), "%"PRIx64, vals[i]);
        CuAssertTrue(tc, strlen(ref) == bw.offset && 0 == memcmp(ref, bw.data, bw.offset));
        binary_free(&bw);
    }
}
static void _id_mt_gen(void *arg) {
    _id_mt_arg *a = (_id_mt_arg *)arg;
    for (int32_t i = 0; i < ID_MT_PER; i++) {
        a->ids[i] = createid();
    }
}
static int _u64_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
// createid 各线程按段领号：多线程合并后没有重复、都非 0，同一线程内严格递增(跨段也是)
static void test_createid_threads(CuTest *tc) {
    _id_mt_arg args[ID_MT_THREADS];
    pthread_t th[ID_MT_THREADS];
    size_t total = (size_t)ID_MT_PER * ID_MT_THREADS;
    uint64_t *all;
    size_t i;
    int32_t t;
    MALLOC(all, sizeof(uint64_t) * total);
    for (t = 0; t < ID_MT_THREADS; t++) {
        args[t].ids = all + (size_t)ID_MT_PER * (size_t)t;
        th[t] = thread_creat(_id_mt_gen, &args[t]);
    }
    for (t = 0; t < ID_MT_THREADS; t++) {
        thread_join(th[t]);
    }
    for (t = 0; t < ID_MT_THREADS; t++) {
        for (i = 1; i < (size_t)ID_MT_PER; i++) {
            CuAssertTrue(tc, args[t].ids[i - 1] < args[t].ids[i]);
        }
    }
    qsort(all, total, sizeof(uint64_t), _u64_cmp);
    CuAssertTrue(tc, 0 != all[0]);
    for (i = 1; i < total; i++) {
        CuAssertTrue(tc, all[i - 1] != all[i]);
    }
    FREE(all);
}
// 尾节点被消费掉大半、剩一截残包时，下个读事件把残包前移，不另建节点(只给一条 iov)，数据不乱
static void test_buffer_tail_realign(CuTest *tc) {
    buffer_ctx buf;
    char pat[MAX_RECV_SIZE], out[MAX_RECV_SIZE];
    size_t nread, i;
    // 残包取 300：32 位下节点按 512 取整、容量更小，残包太长就不满足前移判据
    const size_t keep = 300;
    const size_t eaten = MAX_RECV_SIZE - keep;
    for (i = 0; i < sizeof(pat); i++) {
        pat[i] = (char)(i * 7 + 3);
    }
    buffer_init(&buf);
    CuAssertIntEquals(tc, ERR_OK, buffer_append(&buf, pat, sizeof(pat)));
    CuAssertTrue(tc, eaten == buffer_remove(&buf, out, eaten));
    _fake_rv_reset();
    _fake_rv_want[0] = 1000;
    CuAssertIntEquals(tc, ERR_OK, buffer_from_sock(&buf, 0, &nread, _fake_readv, NULL));
    CuAssertTrue(tc, 1000 == nread);
    CuAssertIntEquals(tc, 1, (int32_t)_fake_rv_niov[0]);
    CuAssertTrue(tc, _fake_rv_offer[0] >= MAX_RECV_SIZE);
    CuAssertTrue(tc, keep + 1000 == buffer_size(&buf));
    CuAssertTrue(tc, keep + 1000 == buffer_copyout(&buf, 0, out, sizeof(out)));
    CuAssertTrue(tc, 0 == memcmp(out, pat + eaten, keep));
    for (i = keep; i < keep + 1000; i++) {
        CuAssertTrue(tc, 'R' == out[i]);
    }
    buffer_free(&buf);
}
// 预留 lens 字节后正好写满 lens 也不扩容(容量另留结尾 NUL 那 1 字节)；lens 取 inc 整倍数及其前后
static void test_binary_init_exact(CuTest *tc) {
    static const size_t lens[] = { 1, 255, 256, 257, 511, 512, 513, 4096 };
    char src[4096];
    binary_ctx bw;
    size_t i, size0;
    memset(src, 'x', sizeof(src));
    for (i = 0; i < ARRAY_SIZE(lens); i++) {
        binary_init_write(&bw, lens[i], 0);
        size0 = bw.size;
        CuAssertTrue(tc, size0 > lens[i]);
        binary_set_binary(&bw, src, lens[i]);
        CuAssertTrue(tc, size0 == bw.size);
        CuAssertTrue(tc, lens[i] == bw.offset);
        binary_free(&bw);
    }
}
// buffer_search 的参照实现：在 [start, e) 里放得下整个 w 的第一个位置，e 的换算同 buffer_search
static int32_t _search_ref(const char *s, size_t n, int32_t ncs, size_t start, size_t end,
                           const char *w, size_t wl) {
    size_t e, p, i;
    if (0 == n || 0 == wl) {
        return ERR_FAILED;
    }
    e = (0 == end || end >= n) ? n : end + 1;
    for (p = start; p < e && wl <= e - p; p++) {
        for (i = 0; i < wl; i++) {
            if (0 != ncs ? tolower((uint8_t)s[p + i]) != tolower((uint8_t)w[i]) : s[p + i] != w[i]) {
                break;
            }
        }
        if (i == wl) {
            return (int32_t)p;
        }
    }
    return ERR_FAILED;
}
// 同一份数据分别放成 1 个节点和 3 个 external 节点，search / copyout / at / drain 与参照实现逐一对拍：
// 单节点走各函数入口的快路径，多节点走慢路径(含 search 按 end 截扫描范围那段)
static void test_buffer_split_equiv(CuTest *tc) {
    const char *flat = "aB;cd\r\nEf;gh;\r\nxyz;AbX\r\n;;tail;aB";
    const char *pats[] = { ";", "\r\n", "aB", "xyz", "Ab;", "tail;aB" };
    size_t n = strlen(flat), cut1 = 5, cut2 = 17;
    buffer_ctx one, three;
    char *p1, *p2, *p3;
    char out[64];
    size_t i, start, end, lens, want, k;
    int32_t ncs, r1, r3, ref;
    buffer_init(&one);
    CuAssertIntEquals(tc, ERR_OK, buffer_append(&one, (void *)flat, n));
    MALLOC(p1, cut1);
    MALLOC(p2, cut2 - cut1);
    MALLOC(p3, n - cut2);
    memcpy(p1, flat, cut1);
    memcpy(p2, flat + cut1, cut2 - cut1);
    memcpy(p3, flat + cut2, n - cut2);
    buffer_init(&three);
    buffer_external(&three, p1, cut1, _ext_free);
    buffer_external(&three, p2, cut2 - cut1, _ext_free);
    buffer_external(&three, p3, n - cut2, _ext_free);
    for (k = 0; k < 2; k++) {
        for (i = 0; i < ARRAY_SIZE(pats); i++) {
            for (ncs = 0; ncs < 2; ncs++) {
                for (start = 0; start <= n; start++) {
                    for (end = 0; end <= n + 1; end++) {
                        ref = _search_ref(flat, n, ncs, start, end, pats[i], strlen(pats[i]));
                        r1 = buffer_search(&one, ncs, start, end, (char *)pats[i], strlen(pats[i]));
                        r3 = buffer_search(&three, ncs, start, end, (char *)pats[i], strlen(pats[i]));
                        CuAssertIntEquals(tc, ref, r1);
                        CuAssertIntEquals(tc, ref, r3);
                    }
                }
            }
        }
        for (start = 0; start <= n + 1; start++) {
            for (lens = 0; lens <= n + 1; lens++) {
                want = start >= n ? 0 : (lens < n - start ? lens : n - start);
                memset(out, 0, sizeof(out));
                CuAssertTrue(tc, want == buffer_copyout(&one, start, out, lens));
                CuAssertTrue(tc, 0 == memcmp(out, flat + (start < n ? start : n), want));
                memset(out, 0, sizeof(out));
                CuAssertTrue(tc, want == buffer_copyout(&three, start, out, lens));
                CuAssertTrue(tc, 0 == memcmp(out, flat + (start < n ? start : n), want));
            }
        }
        for (i = 0; i < n; i++) {
            CuAssertTrue(tc, flat[i] == buffer_at(&one, i));
            CuAssertTrue(tc, flat[i] == buffer_at(&three, i));
        }
        // 第二轮：两边各排掉 3 字节再对拍一遍(单节点走 drain 快路径，三节点走慢路径)
        CuAssertTrue(tc, 3 == buffer_drain(&one, 3));
        CuAssertTrue(tc, 3 == buffer_drain(&three, 3));
        flat += 3;
        n -= 3;
    }
    buffer_free(&one);
    buffer_free(&three);
}
// netaddr_set 的 IPv4 快路径与 inet_pton 逐项对照：接受与否、族、地址字节都得一致；
// 前导零各平台 inet_pton 说法不同，这里只要求与本平台 inet_pton 相同
static void test_netaddr_set_pton(CuTest *tc) {
    static const char *ips[] = {
        "1.2.3.4", "0.0.0.0", "255.255.255.255", "10.0.0.1", "192.168.100.200",
        "01.2.3.4", "1.2.3.04", "00.1.1.1", "256.1.1.1", "1.2.3.256", "1.2.3", "1.2.3.4.5",
        "", ".", "1..2.3", " 1.2.3.4", "1.2.3.4 ", "1.2.3.4a", "a.b.c.d", "1234.1.1.1",
        "::1", "::", "::ffff:1.2.3.4", "fe80::1", "1:2:3:4:5:6:7:8", "1.2.3.4::"
    };
    struct in_addr a4;
    struct in6_addr a6;
    netaddr_ctx addr;
    size_t i;
    int32_t is4, is6, rtn;
    for (i = 0; i < ARRAY_SIZE(ips); i++) {
        is4 = (1 == inet_pton(AF_INET, ips[i], &a4));
        is6 = !is4 && (1 == inet_pton(AF_INET6, ips[i], &a6));
        rtn = netaddr_set(&addr, ips[i], 80);
        CuAssertIntEquals(tc, (is4 || is6) ? ERR_OK : ERR_FAILED, rtn);
        if (is4) {
            CuAssertIntEquals(tc, AF_INET, netaddr_family(&addr));
            CuAssertTrue(tc, 0 == memcmp(&addr.ipv4.sin_addr, &a4, sizeof(a4)));
            CuAssertIntEquals(tc, 80, netaddr_port(&addr));
        } else if (is6) {
            CuAssertIntEquals(tc, AF_INET6, netaddr_family(&addr));
            CuAssertTrue(tc, 0 == memcmp(&addr.ipv6.sin6_addr, &a6, sizeof(a6)));
            CuAssertTrue(tc, 0 == addr.ipv6.sin6_flowinfo);
            CuAssertIntEquals(tc, 80, netaddr_port(&addr));
        }
    }
}
// contenttype 表首、尾与中间几项逐字对照(表改成按偏移取串，偏移错一位就会串到别的类型上)
static void test_contenttype_rows(CuTest *tc) {
    CuAssertStrEquals(tc, "text/h323", contenttype(".323"));
    CuAssertStrEquals(tc, "application/vnd.openxmlformats-officedocument.wordprocessingml.document", contenttype(".docx"));
    CuAssertStrEquals(tc, "video/mp4", contenttype(".MP4"));
    CuAssertStrEquals(tc, "image/x-xwindowdump", contenttype(".xwd"));
    CuAssertStrEquals(tc, "application/x-zip-compressed", contenttype(".zip"));
    CuAssertStrEquals(tc, "application/X-other-1", contenttype(".zip2"));
    CuAssertStrEquals(tc, "application/X-other-1", contenttype(""));
}
// procscnt 缓存后多次取值不变
static void test_procscnt_cached(CuTest *tc) {
    uint32_t n = procscnt();
    CuAssertTrue(tc, n >= 1);
    CuAssertTrue(tc, n == procscnt());
    CuAssertTrue(tc, n == procscnt());
}
// 子进程自成进程组(pgid == 子进程 pid)，popen_close 才能连孙进程一起杀
static void test_popen_pgroup(CuTest *tc) {
#ifndef OS_WIN
    popen_ctx ctx;
    char buf[64];
    int32_t n, wait_ok;
    long pgid;
    CuAssertIntEquals(tc, ERR_OK, popen_startup(&ctx, "ps -o pgid= -p $$", "r"));
    wait_ok = (ERR_OK == popen_waitexit(&ctx, 5000));
    ZERO(buf, sizeof(buf));
    n = popen_read(&ctx, buf, sizeof(buf) - 1, NULL);
    pgid = strtol(buf, NULL, 10);
    int32_t same = ((long)ctx.pid == pgid);
    // 先收拾再断言，理由同 test_popen2
    popen_free(&ctx);
    CuAssertTrue(tc, 0 != wait_ok);
    CuAssertTrue(tc, n > 0);
    CuAssertTrue(tc, 0 != same);
#else
    (void)tc;
#endif
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
    SUITE_ADD_TEST(suite, test_buffer_from_sock_readv_fail);
    SUITE_ADD_TEST(suite, test_buffer_free_resets);
    SUITE_ADD_TEST(suite, test_buffer_hint_after_migrate);
    SUITE_ADD_TEST(suite, test_buffer_node_spare);
    SUITE_ADD_TEST(suite, test_buffer_at_tail_node);
    SUITE_ADD_TEST(suite, test_buffer_fastpath_equiv);
    SUITE_ADD_TEST(suite, test_sfid);
    SUITE_ADD_TEST(suite, test_sfid_seq_exhaust);
    SUITE_ADD_TEST(suite, test_sfid_invalid);
    SUITE_ADD_TEST(suite, test_uuid_rfc_vectors);
    SUITE_ADD_TEST(suite, test_uuid_fromstr_invalid);
    SUITE_ADD_TEST(suite, test_uuid_version_bits);
    SUITE_ADD_TEST(suite, test_uuid_v7_monotonic);
    SUITE_ADD_TEST(suite, test_uuid_v7_threads);
    SUITE_ADD_TEST(suite, test_hash_ring);
    SUITE_ADD_TEST(suite, test_hash_ring_edge);
    SUITE_ADD_TEST(suite, test_netaddr);
    SUITE_ADD_TEST(suite, test_netaddr_extra);
    SUITE_ADD_TEST(suite, test_chan);
    SUITE_ADD_TEST(suite, test_chan_capacity_round);
    SUITE_ADD_TEST(suite, test_hug);
    SUITE_ADD_TEST(suite, test_hug_wakeup_flood);
    SUITE_ADD_TEST(suite, test_timeofday_consistent);
    SUITE_ADD_TEST(suite, test_timer);
    SUITE_ADD_TEST(suite, test_timer_extra);
    SUITE_ADD_TEST(suite, test_timer_ratio);
    SUITE_ADD_TEST(suite, test_load_trend);
    SUITE_ADD_TEST(suite, test_utils_misc);
    SUITE_ADD_TEST(suite, test_popen2);
    SUITE_ADD_TEST(suite, test_popen_close);
    SUITE_ADD_TEST(suite, test_popen_free_reaps);
    SUITE_ADD_TEST(suite, test_log_lv);
    SUITE_ADD_TEST(suite, test_log_slog_filter);
    SUITE_ADD_TEST(suite, test_log_file_mode);
    SUITE_ADD_TEST(suite, test_strptime);
    SUITE_ADD_TEST(suite, test_strptime_invalid);
    SUITE_ADD_TEST(suite, test_strptime_week_rollover);
    SUITE_ADD_TEST(suite, test_strptime_week_neg_yday);
    SUITE_ADD_TEST(suite, test_strptime_neg_yday_with_mon);
    SUITE_ADD_TEST(suite, test_strptime_ampm_overflow);
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
    SUITE_ADD_TEST(suite, test_pool_order);
    SUITE_ADD_TEST(suite, test_pool_capacity_round);
    SUITE_ADD_TEST(suite, test_tda_overflow);
    SUITE_ADD_TEST(suite, test_strtod_c);
    SUITE_ADD_TEST(suite, test_str2u64);
    SUITE_ADD_TEST(suite, test_str2u64_ref);
    SUITE_ADD_TEST(suite, test_fromhex_table);
    SUITE_ADD_TEST(suite, test_memstr_ref);
    SUITE_ADD_TEST(suite, test_memichr_ref);
    SUITE_ADD_TEST(suite, test_ct_memcmp_each_byte);
    SUITE_ADD_TEST(suite, test_pack_float_bytes);
    SUITE_ADD_TEST(suite, test_sfid_init_keeps_ctx);
    SUITE_ADD_TEST(suite, test_mmap_rdwr_roundtrip);
    SUITE_ADD_TEST(suite, test_mmap_empty_bounds);
    SUITE_ADD_TEST(suite, test_mmap_offset);
    SUITE_ADD_TEST(suite, test_mmap_cap_grow);
    SUITE_ADD_TEST(suite, test_mmap_window_keep);
    SUITE_ADD_TEST(suite, test_mmap_beyond_cap);
    SUITE_ADD_TEST(suite, test_mmap_trim_others);
    SUITE_ADD_TEST(suite, test_mmap_rdonly_follow);
    SUITE_ADD_TEST(suite, test_mmap_copy_protect);
    SUITE_ADD_TEST(suite, test_mmap_map_fd);
    SUITE_ADD_TEST(suite, test_mmap_anon);
    SUITE_ADD_TEST(suite, test_mmap_shm);
    SUITE_ADD_TEST(suite, test_mmap_advise_sync);
    SUITE_ADD_TEST(suite, test_mmap_granularity);
    SUITE_ADD_TEST(suite, test_netaddr_ip4_format);
    SUITE_ADD_TEST(suite, test_strptime_wday);
    SUITE_ADD_TEST(suite, test_hash_str);
    SUITE_ADD_TEST(suite, test_binary_set_uint);
    SUITE_ADD_TEST(suite, test_createid_threads);
    SUITE_ADD_TEST(suite, test_buffer_tail_realign);
    SUITE_ADD_TEST(suite, test_hash_ring_incremental);
    SUITE_ADD_TEST(suite, test_popen_waitexit_event);
    SUITE_ADD_TEST(suite, test_binary_init_exact);
    SUITE_ADD_TEST(suite, test_buffer_split_equiv);
    SUITE_ADD_TEST(suite, test_netaddr_set_pton);
    SUITE_ADD_TEST(suite, test_contenttype_rows);
    SUITE_ADD_TEST(suite, test_procscnt_cached);
    SUITE_ADD_TEST(suite, test_popen_pgroup);
}
