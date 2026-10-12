#include "test_base.h"
#include "lib.h"
#include <locale.h>

#define MEMCNT_BATCH   32 // mem_stat 分条计数用例每批并发的线程数
#define MEMCNT_ROUNDS  9 // 批数;总线程数 288 要超过 memory.c 的 MEM_SLOTS(256)
#define MEMCNT_EACH    500 // 每线程的 malloc/free 轮次
#define MEMRE_N 256// _realloc 计数用例每种调用的次数
#define MEMSTR_HAY 48// memstr 随机比对用例的最大源长度
#define MEMSTR_ROUNDS 6000// memstr 随机比对每种 ncs 的轮数

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
// stack_print：有回溯能力的平台至少写出一帧，没有的什么也不写。写进文件再量长度
static void test_stack_print(CuTest *tc) {
    char path[PATH_LENS];
    SNPRINTF(path, sizeof(path), "%s%stest_stack_print.txt", procpath(), PATH_SEPARATORSTR);
    FILE *fp = fopen(path, "w+");
    CuAssertPtrNotNull(tc, fp);
    stack_print(fp);
    // POSIX 那支绕过 stdio 直接写 fd，fseek 到末尾才量得到真实长度
    fseek(fp, 0, SEEK_END);
    long lens = ftell(fp);
    fclose(fp);
    remove(path);
#ifdef HAVE_BACKTRACE
    CuAssertTrue(tc, lens > 0);
#else
    CuAssertTrue(tc, 0 == lens);
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

// CAS 的返回值恰为 0 / 1；期望值用局部变量传，失败后它不能被改写成当前值
// (编译器内建 CAS 失败时会按地址回写期望值，包装层必须按值收)
static void test_atomic_cas_contract(CuTest *tc) {
    atomic_t v = 5;
    atomic_t expect = 3;
    atomic64_t v64 = 5000000000LL;// 超过 32 位，顺带验 64 位版不截断
    atomic64_t expect64 = 3;

    CuAssertIntEquals(tc, 0, ATOMIC_CAS(&v, expect, 7));
    CuAssertTrue(tc, 3 == expect);
    CuAssertTrue(tc, 5 == ATOMIC_GET(&v));
    expect = 5;
    CuAssertIntEquals(tc, 1, ATOMIC_CAS(&v, expect, 7));
    CuAssertTrue(tc, 5 == expect);
    CuAssertTrue(tc, 7 == ATOMIC_GET(&v));

    CuAssertIntEquals(tc, 0, ATOMIC64_CAS(&v64, expect64, 7));
    CuAssertTrue(tc, 3 == expect64);
    CuAssertTrue(tc, 5000000000LL == ATOMIC64_GET(&v64));
    expect64 = 5000000000LL;
    CuAssertIntEquals(tc, 1, ATOMIC64_CAS(&v64, expect64, 7));
    CuAssertTrue(tc, 5000000000LL == expect64);
    CuAssertTrue(tc, 7 == ATOMIC64_GET(&v64));
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
// 超过半块的大块：空链时它当当前块并标满(下个小块另开定长块)；已有当前块时插到它后面，当前块接着切
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
// 半块到整块之间的分配：当前块剩余放不下时单开精确大小的块插在当前块后面，当前块不作废，后面的小块接着切
static void test_mem_arena_half(CuTest *tc) {
    mem_arena a;
    char *r1, *r2, *r3, *s0, *s1, *s2;
    ZERO(&a, sizeof(a));
    s0 = mem_arena_alloc(&a, 24);// 开第一个定长块
    r1 = mem_arena_alloc(&a, 5000);// 当前块剩余放得下，照常切
    CuAssertTrue(tc, r1 == s0 + 24);
    memset(r1, 0x44, 5000);
    r2 = mem_arena_alloc(&a, 5000);// 剩余约 3KB 放不下：单开一块，当前块不动
    memset(r2, 0x55, 5000);
    s1 = mem_arena_alloc(&a, 24);
    CuAssertTrue(tc, s1 == r1 + 5000);// 小块接在 r1 后面，说明当前块没换
    r3 = mem_arena_alloc(&a, 5000);
    memset(r3, 0x66, 5000);
    s2 = mem_arena_alloc(&a, 24);
    CuAssertTrue(tc, s2 == s1 + 24);
    CuAssertTrue(tc, 0x44 == (uint8_t)r1[4999] && 0x55 == (uint8_t)r2[0] && 0x55 == (uint8_t)r2[4999]
                     && 0x66 == (uint8_t)r3[4999]);
    CuAssertTrue(tc, r2 != r3 && (r2 + 5000 <= r3 || r3 + 5000 <= r2));
    mem_arena_free(&a);// 定长块与两块单开块都要收，漏收由收尾内存检查报出
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
 * base 杂项 —— procscnt / nowms / nowsec / sectostr / mstostr / strtots
 * ======================================================================= */
static void test_base_misc(CuTest *tc) {
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

/*
 * once —— 一次性初始化
 */
typedef struct once_arg {
    once_ctx once;
    atomic_t nwin;// 抢到初始化权的次数，必须恰为 1
    atomic_t nmiss;// 返回时还没看到初始化结果的次数，必须为 0
    int32_t value;// 初始化写的值，故意不用原子：靠 once 的发布顺序保证看得见
}once_arg;
// 抢到的那个先睡 20ms 再写值，输家必须等它写完才返回
static void _once_worker(void *arg) {
    once_arg *a = (once_arg *)arg;
    if (once_begin(&a->once)) {
        ATOMIC_ADD(&a->nwin, 1);
        MSLEEP(20);
        a->value = 7;
        once_end(&a->once);
    }
    if (7 != a->value) {
        ATOMIC_ADD(&a->nmiss, 1);
    }
}
// 8 个线程同时抢：只有一个做初始化，其余都等到做完；reset 之后又能再抢到一次
static void test_once(CuTest *tc) {
    once_arg a;
    pthread_t ths[8];
    int32_t i;
    ATOMIC_SET(&a.once, 0);
    ATOMIC_SET(&a.nwin, 0);
    ATOMIC_SET(&a.nmiss, 0);
    a.value = 0;
    for (i = 0; i < 8; i++) {
        ths[i] = thread_creat(_once_worker, &a);
    }
    for (i = 0; i < 8; i++) {
        thread_join(ths[i]);
    }
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&a.nwin));
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&a.nmiss));
    CuAssertTrue(tc, !once_begin(&a.once));// 做完之后再调直接返回 0
    once_reset(&a.once);
    CuAssertTrue(tc, once_begin(&a.once));
    once_end(&a.once);
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
 * base/sfile.h 文件接口 / base/sys.h 进程路径接口
 * isfile / isdir / filesize / readall / procpath
 * 用进程自身路径（procpath() 返回的目录，及其下已知存在的可执行文件）来构造测试输入
 * ======================================================================= */
static void test_base_filesystem(CuTest *tc) {
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
// file_mtime：刚写的文件取到的是近一分钟内的秒数；文件不存在返回 0
static void test_file_mtime(CuTest *tc) {
    char path[PATH_LENS];
    FILE *fp;
    uint64_t mt, now;
    SNPRINTF(path, sizeof(path), "%s%stest_file_mtime.tmp", procpath(), PATH_SEPARATORSTR);
    fp = fopen_cloexec(path, "wb");
    CuAssertPtrNotNull(tc, fp);
    fputs("x", fp);
    fclose(fp);
    mt = file_mtime(path);
    now = nowsec();
    remove(path);
    CuAssertTrue(tc, mt > 0 && mt <= now + 2 && mt + 60 >= now);
    CuAssertTrue(tc, 0 == file_mtime(path));
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

// procscnt 缓存后多次取值不变
static void test_procscnt_cached(CuTest *tc) {
    uint32_t n = procscnt();
    CuAssertTrue(tc, n >= 1);
    CuAssertTrue(tc, n == procscnt());
    CuAssertTrue(tc, n == procscnt());
}

// procscnt_avail：至少 1、不超过整机在线核数，缓存后取值不变；Linux 上还不超过本进程的 CPU 亲和核数(cgroup 配额只会再往下压)
static void test_procscnt_avail(CuTest *tc) {
    uint32_t n = procscnt_avail();
    CuAssertTrue(tc, n >= 1 && n <= procscnt());
    CuAssertTrue(tc, n == procscnt_avail());
#if defined(OS_LINUX)
    cpu_set_t cpus;
    if (0 == sched_getaffinity(0, sizeof(cpus), &cpus)) {// 超过 1024 核时取不到，procscnt_avail 退回整机核数
        CuAssertTrue(tc, n <= (uint32_t)CPU_COUNT(&cpus));
    }
#endif
}

// cpu.max 折成核数：向上取整；"max"、配额或周期为 0、解析不了一律 0(不限)；超过 32 位的封顶
static void test_cgroup_cpu_quota(CuTest *tc) {
    CuAssertIntEquals(tc, 2, (int32_t)_cgroup_cpu_quota("150000 100000\n"));
    CuAssertIntEquals(tc, 1, (int32_t)_cgroup_cpu_quota("100000 100000"));
    CuAssertIntEquals(tc, 1, (int32_t)_cgroup_cpu_quota("50000 100000"));
    CuAssertIntEquals(tc, 4, (int32_t)_cgroup_cpu_quota("400000 100000"));
    CuAssertIntEquals(tc, 0, (int32_t)_cgroup_cpu_quota("max 100000\n"));
    CuAssertIntEquals(tc, 0, (int32_t)_cgroup_cpu_quota("0 100000"));
    CuAssertIntEquals(tc, 0, (int32_t)_cgroup_cpu_quota("100000 0"));
    CuAssertIntEquals(tc, 0, (int32_t)_cgroup_cpu_quota("100000"));
    CuAssertIntEquals(tc, 0, (int32_t)_cgroup_cpu_quota(""));
    CuAssertTrue(tc, UINT32_MAX == _cgroup_cpu_quota("18446744073709551615 1"));
}

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

    /* C 串之间比：lens 取 strlen + 1 连 '\0' 一起比，排序同 strcasecmp，短串是长串前缀时短串小 */
    CuAssertIntEquals(tc, 0, memcasecmp(".HTML", ".html", 6));
    CuAssertIntEquals(tc, -1, memcasecmp(".htm", ".html", 5));
    CuAssertIntEquals(tc, 1, memcasecmp(".json", ".JS", 6));
    CuAssertIntEquals(tc, 1, memcasecmp("A", "_", 2));
    CuAssertIntEquals(tc, 0, memcasecmp("", "", 1));
    CuAssertIntEquals(tc, 1, memcasecmp("\xC1", "a", 2));
    CuAssertIntEquals(tc, 0, memcasecmp("Keep-Alive", "keep-alivexx", 10));
    CuAssertIntEquals(tc, -1, memcasecmp("ab", "abc", 3));
    {
        /* 遇到第一个不同字节就停：短串 3 字节(含 '\0')，lens 给 9 也不会读到第 4 个字节(ASan 下越界即报) */
        char shortstr[3] = { 'a', 'b', '\0' };
        CuAssertIntEquals(tc, -1, memcasecmp(shortstr, "ABCDEFGH", 9));
    }
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
 * secure_zero / csprng_rand
 * ======================================================================= */
static void test_security_helpers(CuTest *tc) {
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

// strtou64：按长度解析十进制无符号整数。收敛了原先散在 scram / http / mysql / coro_utils /
// debug_console / harbor 六处的 strtoXX 用法，故这里把它们各自依赖的边界一次测全
static void test_strtou64(CuTest *tc) {
    uint64_t v;

    // 1) 基本正确性 + 上界恰好命中
    v = 0;
    CuAssertIntEquals(tc, ERR_OK, strtou64("0", 1, UINT64_MAX, &v));
    CuAssertTrue(tc, 0 == v);
    CuAssertIntEquals(tc, ERR_OK, strtou64("65535", 5, UINT16_MAX, &v));
    CuAssertTrue(tc, UINT16_MAX == v);
    CuAssertIntEquals(tc, ERR_FAILED, strtou64("65536", 5, UINT16_MAX, &v));

    // 2) 只吃 lens 个字节，尾部残留不参与——harbor 的 url_decode 场景
    // ("%310" 解码成 "10" 只缩短 lens，缓冲里仍读得到 "1010")
    CuAssertIntEquals(tc, ERR_OK, strtou64("1010", 2, UINT64_MAX, &v));
    CuAssertTrue(tc, 10 == v);

    // 3) strtoXX 会静默收下、这里必须拒的几种：前导空白 / 正负号 / 尾随垃圾 / 内嵌非数字
    CuAssertIntEquals(tc, ERR_FAILED, strtou64(" 5", 2, UINT64_MAX, &v));
    CuAssertIntEquals(tc, ERR_FAILED, strtou64("+5", 2, UINT64_MAX, &v));
    CuAssertIntEquals(tc, ERR_FAILED, strtou64("-1", 2, UINT64_MAX, &v));
    CuAssertIntEquals(tc, ERR_FAILED, strtou64("1x", 2, UINT64_MAX, &v));
    CuAssertIntEquals(tc, ERR_FAILED, strtou64("1 2", 3, UINT64_MAX, &v));
    CuAssertIntEquals(tc, ERR_FAILED, strtou64("abc", 3, UINT64_MAX, &v));

    // 4) 空输入
    CuAssertIntEquals(tc, ERR_FAILED, strtou64("", 0, UINT64_MAX, &v));
    CuAssertIntEquals(tc, ERR_FAILED, strtou64(NULL, 0, UINT64_MAX, &v));
    CuAssertIntEquals(tc, ERR_FAILED, strtou64(NULL, 3, UINT64_MAX, &v));

    // 5) uint64 边界：恰好 UINT64_MAX 收下，末位再 +1 与多一位都要拒
    CuAssertIntEquals(tc, ERR_OK, strtou64("18446744073709551615", 20, UINT64_MAX, &v));
    CuAssertTrue(tc, UINT64_MAX == v);
    CuAssertIntEquals(tc, ERR_FAILED, strtou64("18446744073709551616", 20, UINT64_MAX, &v));
    CuAssertIntEquals(tc, ERR_FAILED, strtou64("99999999999999999999", 20, UINT64_MAX, &v));

    // 6) 前导零不影响判定，也不该被当成溢出
    CuAssertIntEquals(tc, ERR_OK, strtou64("000000000000000000000042", 24, UINT64_MAX, &v));
    CuAssertTrue(tc, 42 == v);

    // 7) 失败时不得写 out —— debug_console 的 loglv 依赖这一点(失败即整条拒绝)
    v = 0x5a5a5a5a;
    CuAssertIntEquals(tc, ERR_FAILED, strtou64("9", 1, 4, &v));
    CuAssertTrue(tc, 0x5a5a5a5a == v);
}

/* =======================================================================
 * base 字符串 / 字节序辅助(bytes / strconv)：与改写前的写法逐一对照
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
    // 起点数正好 16(一轮向量吃完全部起点)：命中在最后一个起点；没命中时交给 _memstr 的只剩不足 wlen 字节
    const char *h17 = "aaaaaaaaaaaaaaaab";
    CuAssertTrue(tc, h17 + 15 == memstr(0, h17, 17, "ab", 2));
    CuAssertTrue(tc, NULL == memstr(0, h17, 17, "ba", 2));
    // 起点数 20：向量一轮比完 0~15，剩下 16~19 交给 _memstr，命中在 18、19
    const char *h21 = "aaaaaaaaaaaaaaaaaaaba";
    CuAssertTrue(tc, h21 + 18 == memstr(0, h21, 21, "ab", 2));
    CuAssertTrue(tc, h21 + 19 == memstr(0, h21, 21, "ba", 2));
}

// memcspn 的参考：逐字节看在不在 what 里
static size_t _memcspn_ref(const unsigned char *p, size_t lens, const char *what, size_t wlens) {
    size_t i, k;
    for (i = 0; i < lens; i++) {
        for (k = 0; k < wlens; k++) {
            if (p[i] == (unsigned char)what[k]) {
                return i;
            }
        }
    }
    return lens;
}
// 随机比对：长度 0~MEMSTR_HAY 覆盖向量主循环、重叠读的尾巴与不足 16 字节的短路径(_memcspn_short)；wlens 0~6 覆盖向量与查表(_memcspn)两支，
// what 里可含 '\0'。源缓冲 lens 之后也填随机字节，越窗读会被比出来
static void test_memcspn_ref(CuTest *tc) {
    const char set[] = { 'a', 'b', '\r', '\n', '\0', ':', 'x' };
    unsigned char hay[MEMSTR_HAY + 8];
    char what[8], buf[24];
    uint32_t seed = 20260929u;
    size_t lens, wlens, i, got;
    int32_t r, bad = 0, nhit = 0, nmiss = 0;
    for (r = 0; r < MEMSTR_ROUNDS; r++) {
        lens = _lcg_next(&seed) % (MEMSTR_HAY + 1);
        wlens = _lcg_next(&seed) % 7;
        // 大多填 'z'(不在 set 里)，命中稀疏，才能走到长段扫描
        for (i = 0; i < sizeof(hay); i++) {
            hay[i] = (unsigned char)(_lcg_next(&seed) % 8 < 6 ? 'z' : set[_lcg_next(&seed) % sizeof(set)]);
        }
        for (i = 0; i < wlens; i++) {
            what[i] = set[_lcg_next(&seed) % sizeof(set)];
        }
        got = memcspn(hay, lens, what, wlens);
        if (got != _memcspn_ref(hay, lens, what, wlens)) {
            bad++;
        }
        if (got < lens) {
            nhit++;
        } else {
            nmiss++;
        }
    }
    CuAssertIntEquals(tc, 0, bad);
    CuAssertTrue(tc, nhit > 0 && nmiss > 0);
    // 确定性边界：命中落在重叠读的尾巴里、窗口外的字节不算、what 含 '\0'、查表分支、wlens 为 0
    memset(buf, 'a', sizeof(buf));
    buf[19] = '\r';
    CuAssertIntEquals(tc, 19, (int)memcspn(buf, 20, "\r\n", 2));
    CuAssertIntEquals(tc, 19, (int)memcspn(buf, 19, "\r\n", 2));
    CuAssertIntEquals(tc, 3, (int)memcspn("abc\0de", 6, "\0", 1));
    CuAssertIntEquals(tc, 2, (int)memcspn("ab:cdefg", 8, "xyz:;,", 6));
    CuAssertIntEquals(tc, 5, (int)memcspn("hello", 5, "", 0));
    // 不足 16 字节的短路径：每个长度、每个命中位置逐一试。命中前填与目标差 1 的字节和高位字节(找零字节的借位只往高处误报，
    // 要证明最低那个命中仍准)，窗口外紧跟一个命中；"\0\r\n" 那组覆盖 1~3 字节拼整数时高位补 0 撞 '\0'
    {
        static const char *sets[] = { "\r", "\r\n", "\0\r\n", "\"\\:x" };
        static const size_t swl[] = { 1, 2, 3, 4 };
        static const unsigned char fill[] = { 0x0c, 0x0e, 0x80, 0xff, 0x01, 0x09, 0x0b, 'z' };
        size_t si, pos, j;
        bad = 0;
        for (si = 0; si < ARRAY_SIZE(sets); si++) {
            for (lens = 0; lens <= 16; lens++) {
                for (pos = 0; pos <= lens; pos++) {
                    for (j = 0; j < lens; j++) {
                        buf[j] = (char)fill[(j + pos) % sizeof(fill)];
                    }
                    buf[lens] = sets[si][0];
                    if (pos < lens) {
                        buf[pos] = sets[si][swl[si] - 1];
                    }
                    got = memcspn(buf, lens, sets[si], swl[si]);
                    if (got != pos || got != _memcspn_ref((const unsigned char *)buf, lens, sets[si], swl[si])) {
                        bad++;
                    }
                }
            }
        }
        CuAssertIntEquals(tc, 0, bad);
    }
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
    // 这些平台用 sbyteswap.c 自己的 ntohll/htonll：每个字节都不对称，换错一段就比得出来
    const uint64_t hv = 0xF1E2D3C4B5A69788ULL;
    const unsigned char hbe[8] = { 0xF1, 0xE2, 0xD3, 0xC4, 0xB5, 0xA6, 0x97, 0x88 };
    uint64_t hn = htonll(hv);
    CuAssertTrue(tc, 0 == memcmp(&hn, hbe, sizeof(hbe)));
    memcpy(&hn, hbe, sizeof(hbe));
    CuAssertTrue(tc, hv == ntohll(hn));
#endif
}

// token_span 与 is_token 同一张 tchar 表：数到第一个非 token 字节为止，':' SP HT CR LF NUL 高位字节都停
static void test_token_span(CuTest *tc) {
    static const char stops[] = { ':', ' ', '\t', '\r', '\n', '\0', '"', '(', '/', '@', (char)0x7f, (char)0x80, (char)0xff };
    char b[8];
    size_t i;
    CuAssertTrue(tc, 0 == token_span(NULL, 0));
    CuAssertTrue(tc, 0 == token_span("abc", 0));
    CuAssertTrue(tc, 4 == token_span("Host: x", 7));
    CuAssertTrue(tc, 14 == token_span("Content-Length:", 15));
    CuAssertTrue(tc, 3 == token_span("abc", 3));
    CuAssertTrue(tc, 0 == token_span(":x", 2));
    CuAssertTrue(tc, 7 == token_span("!#$%&'*+-.^_`|~", 7));
    for (i = 0; i < sizeof(stops); i++) {
        memcpy(b, "ab", 2);
        b[2] = stops[i];
        b[3] = 'c';
        CuAssertTrue(tc, 2 == token_span(b, 4));
        CuAssertTrue(tc, !is_token(b, 4));
    }
}

// strtod_fast 直接测：oks 都走 128 位那层，前四个用到 10^0 以外的表项，最后一个走舍入进位回绕，收下时必须与 strtod_c 逐位相等；
// fails 分别是判不清的半数(1e23 / 2^53+3)与超出 ±64 表范围，必须不收且不写出参。
// 32 位 x87 上快路径整段关掉恒不收，用 "1" 探一下，关掉时 oks 只查"不收也不写"
static void test_strtod_fast(CuTest *tc) {
    static const char *oks[] = { "1e30", "1.5e-50", "123456789012345678e40", "1e64", "7.2057594037927933e+16" };
    static const char *fails[] = { "1e23", "9007199254740995", "1e65" };
    const double sentinel = 12345.0;
    double v = 0.0;
    double ref;
    int32_t on = (ERR_OK == strtod_fast("1", 1, &v));
    size_t i;
    CuAssertTrue(tc, !on || 1.0 == v);
    for (i = 0; i < ARRAY_SIZE(oks); i++) {
        v = sentinel;
        if (ERR_OK == strtod_fast(oks[i], strlen(oks[i]), &v)) {
            ref = strtod_c(oks[i], NULL);
            CuAssertTrue(tc, 0 == memcmp(&v, &ref, sizeof(v)));
        } else {
            CuAssertTrue(tc, !on);
            CuAssertTrue(tc, 0 == memcmp(&v, &sentinel, sizeof(v)));
        }
    }
    for (i = 0; i < ARRAY_SIZE(fails); i++) {
        v = sentinel;
        CuAssertTrue(tc, ERR_FAILED == strtod_fast(fails[i], strlen(fails[i]), &v));
        CuAssertTrue(tc, 0 == memcmp(&v, &sentinel, sizeof(v)));
    }
}

// mul64_128：写死的边界值各平台都跑(MSVC 的 _umul128/__umulh 与 32 位拼接回退都靠它)；有 __int128 时再加随机对照
static void test_mul64_128(CuTest *tc) {
    static const uint64_t cases[][4] = {// a, b, 期望高 64 位, 期望低 64 位
        { 0, 0, 0, 0 },
        { 0, 0xFFFFFFFFFFFFFFFFULL, 0, 0 },
        { 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFEULL, 1 },
        { 0x100000000ULL, 0x100000000ULL, 1, 0 },
        { 0xFFFFFFFFULL, 0xFFFFFFFFULL, 0, 0xFFFFFFFE00000001ULL },
        { 0x100000001ULL, 0x100000001ULL, 1, 0x200000001ULL },
        { 0xFFFFFFFFFFFFFFFFULL, 2, 1, 0xFFFFFFFFFFFFFFFEULL },
        { 0xFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFEULL, 0xFFFFFFFF00000001ULL },
        { 0x8000000000000000ULL, 0x8000000000000000ULL, 0x4000000000000000ULL, 0 },
        { 0xFFFFFFFF00000000ULL, 0xFFFFFFFF00000000ULL, 0xFFFFFFFE00000001ULL, 0 },
        { 0x9E3779B97F4A7C15ULL, 0xD1B54A32D192ED03ULL, 0x819B5574F29E4C7CULL, 0x5750DDE65BB8E53FULL },
    };
    uint64_t hi, lo;
    size_t i;
    for (i = 0; i < ARRAY_SIZE(cases); i++) {
        hi = 0x5A5A5A5A5A5A5A5AULL;
        lo = mul64_128(cases[i][0], cases[i][1], &hi);
        CuAssertTrue(tc, cases[i][2] == hi && cases[i][3] == lo);
    }
#if defined(__SIZEOF_INT128__)
    uint64_t a = 0x243F6A8885A308D3ULL;
    uint64_t b = 0x13198A2E03707344ULL;
    unsigned __int128 p;
    for (i = 0; i < 10000; i++) {
        a ^= a << 13;// xorshift64 造两路伪随机数
        a ^= a >> 7;
        a ^= a << 17;
        b ^= b << 13;
        b ^= b >> 7;
        b ^= b << 17;
        lo = mul64_128(a, b, &hi);
        p = (unsigned __int128)a * b;
        CuAssertTrue(tc, (uint64_t)(p >> 64) == hi && (uint64_t)p == lo);
    }
#endif
}

void test_base(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_memory);
    SUITE_ADD_TEST(suite, test_realloc_edges);
    SUITE_ADD_TEST(suite, test_memory_nc);
    SUITE_ADD_TEST(suite, test_stack_print);
    SUITE_ADD_TEST(suite, test_atomic32);
    SUITE_ADD_TEST(suite, test_atomic64);
    SUITE_ADD_TEST(suite, test_atomic_cas_contract);
    SUITE_ADD_TEST(suite, test_set_ptr_expr_arg);
    SUITE_ADD_TEST(suite, test_round_up_narrow_modulus);
    SUITE_ADD_TEST(suite, test_mem_arena_fast_slow);
    SUITE_ADD_TEST(suite, test_mem_arena_big);
    SUITE_ADD_TEST(suite, test_mem_arena_half);
    SUITE_ADD_TEST(suite, test_u64tostr_dec);
    SUITE_ADD_TEST(suite, test_timer);
    SUITE_ADD_TEST(suite, test_timer_extra);
    SUITE_ADD_TEST(suite, test_timer_ratio);
    SUITE_ADD_TEST(suite, test_timeofday_consistent);
    SUITE_ADD_TEST(suite, test_base_misc);
    SUITE_ADD_TEST(suite, test_once);
    SUITE_ADD_TEST(suite, test_strptime);
    SUITE_ADD_TEST(suite, test_strptime_invalid);
    SUITE_ADD_TEST(suite, test_strptime_week_rollover);
    SUITE_ADD_TEST(suite, test_strptime_week_neg_yday);
    SUITE_ADD_TEST(suite, test_strptime_neg_yday_with_mon);
    SUITE_ADD_TEST(suite, test_strptime_ampm_overflow);
    SUITE_ADD_TEST(suite, test_timestr_out_of_range);
    SUITE_ADD_TEST(suite, test_misc_helpers);
    SUITE_ADD_TEST(suite, test_base_filesystem);
    SUITE_ADD_TEST(suite, test_file_mtime);
    SUITE_ADD_TEST(suite, test_strptime_wday);
    SUITE_ADD_TEST(suite, test_hash_str);
    SUITE_ADD_TEST(suite, test_procscnt_cached);
    SUITE_ADD_TEST(suite, test_procscnt_avail);
    SUITE_ADD_TEST(suite, test_cgroup_cpu_quota);
    SUITE_ADD_TEST(suite, test_pack_unpack);
    SUITE_ADD_TEST(suite, test_mem_helpers);
    SUITE_ADD_TEST(suite, test_safe_fill_str);
    SUITE_ADD_TEST(suite, test_str_helpers);
    SUITE_ADD_TEST(suite, test_format_va);
    SUITE_ADD_TEST(suite, test_security_helpers);
    SUITE_ADD_TEST(suite, test_strtod_c);
    SUITE_ADD_TEST(suite, test_strtou64);
    SUITE_ADD_TEST(suite, test_fromhex_table);
    SUITE_ADD_TEST(suite, test_memstr_ref);
    SUITE_ADD_TEST(suite, test_memcspn_ref);
    SUITE_ADD_TEST(suite, test_memichr_ref);
    SUITE_ADD_TEST(suite, test_pack_float_bytes);
    SUITE_ADD_TEST(suite, test_token_span);
    SUITE_ADD_TEST(suite, test_strtod_fast);
    SUITE_ADD_TEST(suite, test_mul64_128);
}
