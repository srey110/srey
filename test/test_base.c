#include "test_base.h"
#include "lib.h"

#define MEMCNT_BATCH   32 // mem_stat 分条计数用例每批并发的线程数
#define MEMCNT_ROUNDS  9 // 批数;总线程数 288 要超过 memory.c 的 MEM_SLOTS(256)
#define MEMCNT_EACH    500 // 每线程的 malloc/free 轮次
#define MEMRE_N 256// _realloc 计数用例每种调用的次数

/* -----------------------------------------------------------------------
 * 内存宏：MALLOC / CALLOC / REALLOC / FREE
 * ----------------------------------------------------------------------- */
static void test_memory(CuTest *tc) {
    /* MALLOC 分配，FREE 释放 */
    /* 分配器失败即 exit（memory.c），返回值不可能是 NULL，判空是恒真断言 */
    int *pi;
    MALLOC(pi, sizeof(int));
    *pi = 42;
    CuAssertIntEquals(tc, 42, *pi);
    FREE(pi);

    /* CALLOC 分配并清零 */
    int *buf;
    int i;
    CALLOC(buf, 8, sizeof(int));
    for (i = 0; i < 8; i++) {
        CuAssertIntEquals(tc, 0, buf[i]);
    }

    /* REALLOC 扩容，原数据保留。先写进可区分的值再读回来——
       源数据全是 CALLOC 的 0 时，"保留了"和"重新清零了"分不开 */
    for (i = 0; i < 8; i++) {
        buf[i] = i * 7 + 1;
    }
    int *nbuf;
    REALLOC(nbuf, buf, 16 * sizeof(int));
    for (i = 0; i < 8; i++) {
        CuAssertIntEquals(tc, i * 7 + 1, nbuf[i]);
    }
    FREE(nbuf);
}

/* -----------------------------------------------------------------------
 * _realloc 的四种入参：(NULL,n) 记一次分配，(p,0) 记一次释放，(NULL,0) 与 (p,n) 不计数。
 * 每种连做 MEMRE_N 次，增量落在 [N, 2N) 才算对：少了是漏记，到 2N 是记重；
 * 别的线程(日志线程)也在分配，所以不断言恰好等于 N，只要求噪声远小于 N
 * ----------------------------------------------------------------------- */
#if MEMORY_CHECK
// 增量 d 恰好对应 N 次计数(允许少量外部噪声)
static int32_t _memre_hit(uint64_t d) {
    return d >= MEMRE_N && d < 2 * MEMRE_N;
}
#endif
static void test_realloc_edges(CuTest *tc) {
    void *ps[MEMRE_N];
    int32_t i, bad = 0;
#if MEMORY_CHECK
    uint64_t a0, f0, a1, f1;
#endif

    /* (NULL, n)：等同 malloc，返回可写的新块 */
#if MEMORY_CHECK
    mem_stat(&a0, &f0);
#endif
    for (i = 0; i < MEMRE_N; i++) {
        ps[i] = _realloc(NULL, 16);
        if (NULL == ps[i]) {
            bad++;
        } else {
            memset(ps[i], i & 0xff, 16);
        }
    }
    CuAssertIntEquals(tc, 0, bad);
#if MEMORY_CHECK
    mem_stat(&a1, &f1);
    CuAssertTrue(tc, _memre_hit(a1 - a0));
    CuAssertTrue(tc, f1 - f0 < MEMRE_N);
#endif

    /* (p, n>0)：改大小不计数，原内容保留 */
#if MEMORY_CHECK
    mem_stat(&a0, &f0);
#endif
    for (i = 0; i < MEMRE_N; i++) {
        ps[i] = _realloc(ps[i], 64);
        if (NULL == ps[i] || (unsigned char)(i & 0xff) != ((unsigned char *)ps[i])[15]) {
            bad++;
        }
    }
    CuAssertIntEquals(tc, 0, bad);
#if MEMORY_CHECK
    mem_stat(&a1, &f1);
    CuAssertTrue(tc, a1 - a0 < MEMRE_N);
    CuAssertTrue(tc, f1 - f0 < MEMRE_N);
#endif

    /* (p, 0)：等同 free，返回 NULL */
#if MEMORY_CHECK
    mem_stat(&a0, &f0);
#endif
    for (i = 0; i < MEMRE_N; i++) {
        if (NULL != _realloc(ps[i], 0)) {
            bad++;
        }
    }
    CuAssertIntEquals(tc, 0, bad);
#if MEMORY_CHECK
    mem_stat(&a1, &f1);
    CuAssertTrue(tc, a1 - a0 < MEMRE_N);
    CuAssertTrue(tc, _memre_hit(f1 - f0));
#endif

    /* (NULL, 0)：什么都不做，返回 NULL */
#if MEMORY_CHECK
    mem_stat(&a0, &f0);
#endif
    for (i = 0; i < MEMRE_N; i++) {
        if (NULL != _realloc(NULL, 0)) {
            bad++;
        }
    }
    CuAssertIntEquals(tc, 0, bad);
#if MEMORY_CHECK
    mem_stat(&a1, &f1);
    CuAssertTrue(tc, a1 - a0 < MEMRE_N);
    CuAssertTrue(tc, f1 - f0 < MEMRE_N);
#endif
}

// _realloc_nc / _free_nc：返回值口径同 _realloc / _free，但四种入参都不计数；
// mem_count_add 把调用方攒的次数并进全局计数。nalloc 与 nfree 各加 MEMRE_N，存活数不变，
// 自己分配的也都自己放掉，退出时的内存检查照样为 0。MEMORY_CHECK 关闭时只核返回值
static void test_memory_nc(CuTest *tc) {
    void *ps[MEMRE_N];
    int32_t i, bad = 0;
#if MEMORY_CHECK
    uint64_t a0, f0, a1, f1;
#endif

    // (NULL, n)：返回可写的新块，不计数
#if MEMORY_CHECK
    mem_stat(&a0, &f0);
#endif
    for (i = 0; i < MEMRE_N; i++) {
        ps[i] = _realloc_nc(NULL, 16);
        if (NULL == ps[i]) {
            bad++;
        } else {
            memset(ps[i], i & 0xff, 16);
        }
    }
    CuAssertIntEquals(tc, 0, bad);
#if MEMORY_CHECK
    mem_stat(&a1, &f1);
    CuAssertTrue(tc, a1 - a0 < MEMRE_N);
    CuAssertTrue(tc, f1 - f0 < MEMRE_N);
#endif

    // (p, n>0)：原内容保留，不计数
#if MEMORY_CHECK
    mem_stat(&a0, &f0);
#endif
    for (i = 0; i < MEMRE_N; i++) {
        ps[i] = _realloc_nc(ps[i], 64);
        if (NULL == ps[i] || (unsigned char)(i & 0xff) != ((unsigned char *)ps[i])[15]) {
            bad++;
        }
    }
    CuAssertIntEquals(tc, 0, bad);
#if MEMORY_CHECK
    mem_stat(&a1, &f1);
    CuAssertTrue(tc, a1 - a0 < MEMRE_N);
    CuAssertTrue(tc, f1 - f0 < MEMRE_N);
#endif

    // (p, 0)：释放并返回 NULL，不计数
#if MEMORY_CHECK
    mem_stat(&a0, &f0);
#endif
    for (i = 0; i < MEMRE_N; i++) {
        if (NULL != _realloc_nc(ps[i], 0)) {
            bad++;
        }
    }
    CuAssertIntEquals(tc, 0, bad);
#if MEMORY_CHECK
    mem_stat(&a1, &f1);
    CuAssertTrue(tc, a1 - a0 < MEMRE_N);
    CuAssertTrue(tc, f1 - f0 < MEMRE_N);
#endif

    // (NULL, 0)：什么都不做，返回 NULL
#if MEMORY_CHECK
    mem_stat(&a0, &f0);
#endif
    for (i = 0; i < MEMRE_N; i++) {
        if (NULL != _realloc_nc(NULL, 0)) {
            bad++;
        }
    }
    CuAssertIntEquals(tc, 0, bad);
#if MEMORY_CHECK
    mem_stat(&a1, &f1);
    CuAssertTrue(tc, a1 - a0 < MEMRE_N);
    CuAssertTrue(tc, f1 - f0 < MEMRE_N);
#endif

    // _free_nc：释放不计数，NULL 什么都不做
    for (i = 0; i < MEMRE_N; i++) {
        ps[i] = _realloc_nc(NULL, 8);
    }
#if MEMORY_CHECK
    mem_stat(&a0, &f0);
#endif
    for (i = 0; i < MEMRE_N; i++) {
        _free_nc(ps[i]);
        _free_nc(NULL);
    }
#if MEMORY_CHECK
    mem_stat(&a1, &f1);
    CuAssertTrue(tc, a1 - a0 < MEMRE_N);
    CuAssertTrue(tc, f1 - f0 < MEMRE_N);
#endif

    // mem_count_add：两个参数各自只加自己那一项；两次合起来 nalloc、nfree 各加 MEMRE_N，存活数不变
#if MEMORY_CHECK
    mem_stat(&a0, &f0);
#endif
    mem_count_add(MEMRE_N, 0);
#if MEMORY_CHECK
    mem_stat(&a1, &f1);
    CuAssertTrue(tc, _memre_hit(a1 - a0));
    CuAssertTrue(tc, f1 - f0 < MEMRE_N);
    mem_stat(&a0, &f0);
#endif
    mem_count_add(0, MEMRE_N);
#if MEMORY_CHECK
    mem_stat(&a1, &f1);
    CuAssertTrue(tc, a1 - a0 < MEMRE_N);
    CuAssertTrue(tc, _memre_hit(f1 - f0));
#endif
}

/* -----------------------------------------------------------------------
 * 32 位原子操作：SET / ADD / CAS / GET
 * ----------------------------------------------------------------------- */
static void test_atomic32(CuTest *tc) {
    atomic_t v = 0;

    /* ATOMIC_SET 写入并读回 */
    ATOMIC_SET(&v, 10);
    CuAssertTrue(tc, 10 == ATOMIC_GET(&v));

    /* ATOMIC_ADD 返回旧值，v 变为 11 */
    atomic_t old = ATOMIC_ADD(&v, 1);
    CuAssertTrue(tc, 10 == old);
    CuAssertTrue(tc, 11 == ATOMIC_GET(&v));

    /* ATOMIC_CAS 成功：旧值匹配时更新 */
    CuAssertTrue(tc, ATOMIC_CAS(&v, 11, 20));
    CuAssertTrue(tc, 20 == ATOMIC_GET(&v));

    /* ATOMIC_CAS 失败：旧值不匹配时保持不变 */
    CuAssertTrue(tc, !ATOMIC_CAS(&v, 0, 99));
    CuAssertTrue(tc, 20 == ATOMIC_GET(&v));

    /* 减法：ADD 负数 */
    ATOMIC_ADD(&v, -5);
    CuAssertTrue(tc, 15 == ATOMIC_GET(&v));
}

/* -----------------------------------------------------------------------
 * 64 位原子操作：SET / ADD / CAS / GET
 * ----------------------------------------------------------------------- */
static void test_atomic64(CuTest *tc) {
    atomic64_t v = 0;

    ATOMIC64_SET(&v, 1000000000LL);
    CuAssertTrue(tc, 1000000000LL == ATOMIC64_GET(&v));

    atomic64_t old = ATOMIC64_ADD(&v, 1);
    CuAssertTrue(tc, 1000000000LL == old);
    CuAssertTrue(tc, 1000000001LL == ATOMIC64_GET(&v));

    CuAssertTrue(tc, ATOMIC64_CAS(&v, 1000000001LL, 2000000000LL));
    CuAssertTrue(tc, 2000000000LL == ATOMIC64_GET(&v));

    CuAssertTrue(tc, !ATOMIC64_CAS(&v, 0, 99));
    CuAssertTrue(tc, 2000000000LL == ATOMIC64_GET(&v));
}

// SET_PTR 的宏体解引用必须括起来。改前是 (*ptr)，传表达式时 * 先于 + 结合：
// SET_PTR(base + 1, v) 展开成 (*base + 1) = v，赋值目标整个错位（多数情况直接编译失败）。
// 本用例传的就是表达式实参，改前编译不过，故它同时是编译期与运行期回归
static void test_set_ptr_expr_arg(CuTest *tc) {
    int32_t buf[3] = { 0, 0, 0 };
    int32_t *base = buf;

    SET_PTR(base + 1, 42);
    CuAssertIntEquals(tc, 0, buf[0]);
    CuAssertIntEquals(tc, 42, buf[1]);
    CuAssertIntEquals(tc, 0, buf[2]);

    // 三元表达式实参同理
    SET_PTR(1 ? base + 2 : NULL, 7);
    CuAssertIntEquals(tc, 7, buf[2]);

    // NULL 守卫仍生效
    int32_t *nil = NULL;
    SET_PTR(nil, 99);
    CuAssertTrue(tc, NULL == nil);
}

/* -----------------------------------------------------------------------
 * mem_stat 的分条计数：288 个线程分 9 批、每批 32 个并发 malloc/free，一个计数都不许丢。
 * 总数超过 MEM_SLOTS，前面的线程落独占格(本线程读写自增)，后面的落共享兜底格(原子自增)，
 * 两条路径都走到，断言对两者都成立。分批是为了并发线程数有界(32 位进程的地址空间放不下几百个栈)。
 * 断言用 >= 而不是 ==：本进程还有别的线程(日志线程等)也在分配，增量只会偏大；
 * 而要防的回归恰好是"少算"——共享格用裸自增就会丢
 * MEMORY_CHECK 关掉时 mem_stat 恒写 0，增量必为 0，断言必挂，所以整块随开关编译
 * ----------------------------------------------------------------------- */
#if MEMORY_CHECK
static void _memcnt_worker(void *arg) {
    (void)arg;
    void *p;
    int32_t i;
    for (i = 0; i < MEMCNT_EACH; i++) {
        MALLOC(p, 32);
        FREE(p);
    }
}
static void test_mem_stat_striped(CuTest *tc) {
    uint64_t a0, f0, a1, f1;
    pthread_t th[MEMCNT_BATCH];
    int32_t i, r;
    mem_stat(&a0, &f0);
    for (r = 0; r < MEMCNT_ROUNDS; r++) {
        for (i = 0; i < MEMCNT_BATCH; i++) {
            th[i] = thread_creat(_memcnt_worker, NULL);
        }
        for (i = 0; i < MEMCNT_BATCH; i++) {
            thread_join(th[i]);
        }
    }
    mem_stat(&a1, &f1);
    const uint64_t want = (uint64_t)MEMCNT_ROUNDS * MEMCNT_BATCH * MEMCNT_EACH;
    CuAssertTrue(tc, a1 - a0 >= want);
    CuAssertTrue(tc, f1 - f0 >= want);
}
#endif//MEMORY_CHECK

// 这个用例会把 memory.c 的计数槽位一次性用光(只增不回收), 之后本进程新建的线程
// 全落到共享的兜底格。集成阶段那十几条 loader 线程才是分条计数最该压的场景,
// 所以它单开一个入口, 由 main 排在集成阶段之后跑
void test_base_slots(CuSuite *suite) {
#if MEMORY_CHECK
    SUITE_ADD_TEST(suite, test_mem_stat_striped);
#else
    (void)suite;
#endif
}
// ROUND_UP 的掩码按 size_t 算。改前掩码取 n 自己的类型：n 是比 size_t 窄的无符号量时
// ~(n-1) 零扩展，s 的高位被一起清掉，取整结果反而小于 s —— 拿这个长度去分配就是一块不够大的内存
static void test_round_up_narrow_modulus(CuTest *tc) {
    uint32_t align = 8;
    CuAssertTrue(tc, 16 == ROUND_UP((size_t)9, align));
    CuAssertTrue(tc, 16 == ROUND_UP((size_t)9, 8));/* 有符号字面量的老用法不变 */
#if SIZE_MAX > 0xFFFFFFFFu
    /* s 超过 32 位：改前结果被截到低 32 位，返回 8 */
    size_t big = ((size_t)1 << 33) + 1;
    CuAssertTrue(tc, (((size_t)1 << 33) + 8) == ROUND_UP(big, align));
#endif
}
// mem_arena 快路径在头文件里内联、慢路径单拆：连续切小块都落在同一块里、首尾相接，切到放不下才开新块
static void test_mem_arena_fast_slow(CuTest *tc) {
    mem_arena a;
    char *p, *prev = NULL;
    int32_t i, newblk = 0;
    ZERO(&a, sizeof(a));
    for (i = 0; i < 2000; i++) {
        p = mem_arena_alloc(&a, 13);
        CuAssertTrue(tc, 0 == ((uintptr_t)p & 7));
        memset(p, 0x5a, 13);
        if (NULL != prev && p != prev + 16) {
            newblk++;// 13 取整到 16：同块里必是紧挨着的，不挨着即换了块
        }
        prev = p;
    }
    CuAssertTrue(tc, newblk >= 2 && newblk <= 5);// 2000 * 16 / (8192 - 8) 约 3.9 块
    p = mem_arena_alloc(&a, 0);
    CuAssertPtrNotNull(tc, p);
    mem_arena_free(&a);
    CuAssertTrue(tc, NULL == a.cur && 0 == a.off && 0 == a.cap);
}
// 一整块装不下的大块：空链时它当当前块并标满(下个小块另开定长块)；已有当前块时插到它后面，当前块接着切
static void test_mem_arena_big(CuTest *tc) {
    mem_arena a;
    char *big, *big2, *s1, *s2, *s3;
    ZERO(&a, sizeof(a));
    big = mem_arena_alloc(&a, 10000);
    CuAssertTrue(tc, 0 == ((uintptr_t)big & 7));
    memset(big, 0x11, 10000);
    CuAssertTrue(tc, a.off == a.cap);// 空链上的大块即当前块，且已用满
    s1 = mem_arena_alloc(&a, 24);
    memset(s1, 0x22, 24);
    s2 = mem_arena_alloc(&a, 24);
    CuAssertTrue(tc, s2 == s1 + 24);// 新开的定长块里首尾相接
    CuAssertTrue(tc, 0x11 == (uint8_t)big[0] && 0x11 == (uint8_t)big[9999]);// 大块没被后来的小块覆盖
    big2 = mem_arena_alloc(&a, 9000);
    memset(big2, 0x33, 9000);
    s3 = mem_arena_alloc(&a, 24);
    CuAssertTrue(tc, s3 == s2 + 24);// 大块插在当前块后面，当前块没换，小块接着切
    CuAssertTrue(tc, 0x22 == (uint8_t)s1[0] && 0x33 == (uint8_t)big2[8999]);
    mem_arena_free(&a);// 大、定长、大三块都要收，漏收由收尾内存检查报出
    CuAssertTrue(tc, NULL == a.cur && 0 == a.off && 0 == a.cap);
}
// u64tostr 十进制与 snprintf("%llu") 逐字节、返回长度都一致；输出缓冲先填满非数字，多写少写都看得出
static void _u64tostr_eq(CuTest *tc, uint64_t v) {
    char want[32], got[32];
    int n = snprintf(want, sizeof(want), "%llu", (unsigned long long)v);
    memset(got, 'x', sizeof(got));
    CuAssertTrue(tc, (size_t)n == u64tostr(got, v, 10));
    CuAssertStrEquals(tc, want, got);
    CuAssertTrue(tc, 'x' == got[n + 1]);
}
// u64tostr 十进制按两位一查表写：0、每个位数的首尾(10^k - 1、10^k)、UINT64_MAX 与一批伪随机值；
// 16 进制与 i64tostr 的负数顺带核一下
static void test_u64tostr_dec(CuTest *tc) {
    uint64_t p = 1, x = 88172645463325252ULL;
    char want[32], got[32];
    int32_t k, i;
    for (i = 0; i < 1000; i++) {
        _u64tostr_eq(tc, (uint64_t)i);
    }
    for (k = 1; k <= 19; k++) {
        p *= 10;
        _u64tostr_eq(tc, p - 1);
        _u64tostr_eq(tc, p);
        _u64tostr_eq(tc, p + 1);
    }
    _u64tostr_eq(tc, UINT64_MAX);
    _u64tostr_eq(tc, UINT64_MAX - 1);
    // xorshift64 伪随机，再右移随机位数，各种位数都会出现
    for (i = 0; i < 100000; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        _u64tostr_eq(tc, x >> (x & 63));
    }
    snprintf(want, sizeof(want), "%lld", (long long)INT64_MIN);
    CuAssertTrue(tc, strlen(want) == i64tostr(got, INT64_MIN, 10));
    CuAssertStrEquals(tc, want, got);
    CuAssertTrue(tc, 2 == i64tostr(got, -7, 10));
    CuAssertStrEquals(tc, "-7", got);
    CuAssertTrue(tc, 16 == u64tostr(got, UINT64_MAX, 16));
    CuAssertStrEquals(tc, "ffffffffffffffff", got);
    CuAssertTrue(tc, 1 == u64tostr(got, 0, 16));
    CuAssertStrEquals(tc, "0", got);
}

void test_base(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_memory);
    SUITE_ADD_TEST(suite, test_realloc_edges);
    SUITE_ADD_TEST(suite, test_memory_nc);
    SUITE_ADD_TEST(suite, test_atomic32);
    SUITE_ADD_TEST(suite, test_atomic64);
    SUITE_ADD_TEST(suite, test_set_ptr_expr_arg);
    SUITE_ADD_TEST(suite, test_round_up_narrow_modulus);
    SUITE_ADD_TEST(suite, test_mem_arena_fast_slow);
    SUITE_ADD_TEST(suite, test_mem_arena_big);
    SUITE_ADD_TEST(suite, test_u64tostr_dec);
}
