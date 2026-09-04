#include "test_minicoro.h"
#include "lib.h"
#include "srey/minicoro.h"
#include <fenv.h>

// fenv 的读写不能被编译器当普通计算折叠掉。三家写法不同：clang 认标准的 STDC 形式，
// MSVC 只认自己那条 fenv_access（给它 STDC 形式会报 C4068 未知杂注），GCC 两条都不认
#if defined(__clang__)
    #pragma STDC FENV_ACCESS ON
#elif defined(_MSC_VER)
    #pragma fenv_access (on)
#endif
// 保存 FP 控制寄存器的只有 64 位的 ASM 后端（aarch64 / SysV x86_64 / Win64），32 位的 ASM
// 变体与 fibers / asyncify 都没加。后端由 minicoro.h 公开段给出，不必自己判平台
#if defined(MCO_USE_ASM) && (defined(ARCH_X64) || defined(ARCH_ARM64))
    #define TEST_MCO_FPU 1
#endif

#ifdef MCO_HAS_STACK_GUARD

// 覆盖 minicoro 的本地补丁：栈底守卫字。
// 上游唯一的哨兵 magic_number 在 mco_coro 末尾，离栈底隔着 storage 和 _mco_context，
// 溢出会先砸掉 back_ctx 而它仍完好；补丁在栈底正下方另置一整带守卫字，任何越过栈底的写都先碰它。
// mco_yield 判定成功时不返回（协程挂起），所以用 mco_status 判定：DEAD=被拦下并跑完，SUSPENDED=漏网。
// 踩的是协程自己那块分配里的守卫槽，不越界、不污染堆。

static int32_t g_stomp_off;// 本轮往 stack_base 下方第几个字节写
static mco_result g_yield_rtn;// 协程内 mco_yield 的返回值

static void _mco_stomp_entry(mco_coro *co) {
    unsigned char *base = (unsigned char *)co->stack_base;
    base[-g_stomp_off] = 0xAA;
    g_yield_rtn = mco_yield(co);// 判定失败才会返回，成功则挂起在这
}
static void _mco_clean_entry(mco_coro *co) {
    g_yield_rtn = mco_yield(co);
}
// 守卫整带都要拦，每个 size_t 字都填都查：只填查一个字（早期写法）时 [-8] 与 [-1] 会漏网。
// 所有用例都先跑完再断言：CuAssert 失败走 longjmp，夹在中间会漏掉 mco_destroy，
// 一次守卫回归就会同时报出一个真失败和一个假的 MEMORY_CHECK 泄漏
static void test_mco_stack_guard(CuTest *tc) {
    static const int32_t offs[] = { 1, 8, 9, 16, 33, 40, 64 };
    mco_desc desc;
    mco_coro *co;
    mco_result rres, rdes;
    mco_state st;
    size_t i;
    // offs 是按 64 字节守卫带挑的，带宽改了这里得跟着改：
    // 最大那几个偏移会落到守卫下方的 storage / back_ctx 上，变成踩上下文而不是踩守卫
    CuAssertIntEquals(tc, 64, (int32_t)MCO_STACK_GUARD_SIZE);
    for (i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        g_stomp_off = offs[i];
        g_yield_rtn = MCO_SUCCESS;
        desc = mco_desc_init(_mco_stomp_entry, 0);
        CuAssertTrue(tc, MCO_SUCCESS == mco_create(&co, &desc));
        // 协程踩完守卫后 mco_yield 判定失败并返回，协程体跑完转 DEAD；
        // mco_resume 回到线程栈后再查一次守卫，同样报 STACK_OVERFLOW
        rres = mco_resume(co);
        st = mco_status(co);
        rdes = mco_destroy(co);
        CuAssertTrue(tc, MCO_STACK_OVERFLOW == rres);
        CuAssertTrue(tc, MCO_STACK_OVERFLOW == g_yield_rtn);
        CuAssertTrue(tc, MCO_DEAD == st);
        CuAssertTrue(tc, MCO_SUCCESS == rdes);
    }
}
// 反向对照：不踩守卫时不得误报，否则每次 yield 都会假阳性
static void test_mco_stack_guard_clean(CuTest *tc) {
    mco_desc desc = mco_desc_init(_mco_clean_entry, 0);
    mco_coro *co;
    g_yield_rtn = MCO_STACK_OVERFLOW;
    CuAssertTrue(tc, MCO_SUCCESS == mco_create(&co, &desc));
    mco_result r1 = mco_resume(co);
    mco_state st1 = mco_status(co);// 正常挂在 mco_yield 里
    mco_result r2 = mco_resume(co);// 唤醒跑完
    mco_state st2 = mco_status(co);
    mco_result rdes = mco_destroy(co);
    CuAssertTrue(tc, MCO_SUCCESS == r1);
    CuAssertTrue(tc, MCO_SUSPENDED == st1);
    CuAssertTrue(tc, MCO_SUCCESS == r2);
    CuAssertTrue(tc, MCO_SUCCESS == g_yield_rtn);
    CuAssertTrue(tc, MCO_DEAD == st2);
    CuAssertTrue(tc, MCO_SUCCESS == rdes);
}
#endif//MCO_HAS_STACK_GUARD

#ifdef TEST_MCO_FPU
// FP 控制寄存器按 ABI 归被调用方保存：协程里改了舍入模式，既不能漏回线程，
// 也不能在自己 yield 一趟后丢掉。修复前第一条断言就会挂
static int32_t g_co_round;
static void _mco_fpcr_entry(mco_coro *co) {
    fesetround(FE_UPWARD);
    mco_yield(co);
    g_co_round = fegetround();
}
static void test_mco_fpu_isolated(CuTest *tc) {
    fesetround(FE_TONEAREST);
    g_co_round = -1;
    mco_desc desc = mco_desc_init(_mco_fpcr_entry, 0);
    mco_coro *co;
    CuAssertTrue(tc, MCO_SUCCESS == mco_create(&co, &desc));
    mco_result r1 = mco_resume(co);// 协程改成 FE_UPWARD 后 yield
    int32_t thread_round = fegetround();
    mco_result r2 = mco_resume(co);
    mco_state st = mco_status(co);
    mco_result rdes = mco_destroy(co);
    // 断言之前先复位：失败走 longjmp 会跳过收尾，把 FE_UPWARD 漏给后面几个套件的 double 运算
    fesetround(FE_TONEAREST);
    CuAssertTrue(tc, MCO_SUCCESS == r1);
    CuAssertIntEquals(tc, FE_TONEAREST, thread_round);// 没漏回线程
    CuAssertTrue(tc, MCO_SUCCESS == r2);
    CuAssertIntEquals(tc, FE_UPWARD, g_co_round);// 协程自己的模式跨 yield 保住
    CuAssertTrue(tc, MCO_DEAD == st);
    CuAssertTrue(tc, MCO_SUCCESS == rdes);
}
#endif

// 覆盖 minicoro 的本地补丁"先校验后分配"：coro_size 撑爆加法时 mco_desc_init 置 0，
// mco_create 必须在 alloc 之前就拒掉；校验挪回分配之后（上游写法）时那个 0 字节的块
// 会被 mco_init 按 sizeof(mco_coro) 清零 → 堆越界。用自带的计数分配器断言它没被调过。
// 只在 coro_size 含 stack_size 的后端有意义：Windows fibers 的 coro_size 是定长的
// （栈由 CreateFiberEx 自己管），撑不爆，也就没有这个守卫
#if defined(MCO_USE_ASM) || defined(MCO_USE_UCONTEXT)
static int32_t g_mco_allocs;
static void _mco_noop_entry(mco_coro *co) {
    (void)co;
}
static void *_mco_count_alloc(size_t size, void *ud) {
    (void)ud;
    g_mco_allocs++;
    return malloc(size);
}
static void _mco_count_dealloc(void *ptr, size_t size, void *ud) {
    (void)size;
    (void)ud;
    free(ptr);
}
static void test_mco_desc_overflow_rejected(CuTest *tc) {
    // 入参必须已经 16 对齐：mco_desc_init 会先 _mco_align_forward(stack_size, 16)，
    // 传 (size_t)-1 的话 (addr+15) 先回绕成 14、再 &~15 得 0，coro_size 反而算出合法值，
    // 拒收改由 stack_size < MCO_MIN_STACK_SIZE 那道完成，走不到这里要测的守卫
    mco_desc desc = mco_desc_init(_mco_noop_entry, (size_t)-16);
    mco_coro *co = (mco_coro *)(uintptr_t)0x1;// 非 NULL 哨兵：失败时 mco_create 必须写回 NULL
    g_mco_allocs = 0;
    desc.alloc_cb = _mco_count_alloc;
    desc.dealloc_cb = _mco_count_dealloc;
    CuAssertTrue(tc, 0 == desc.coro_size);
    CuAssertIntEquals(tc, MCO_INVALID_ARGUMENTS, mco_create(&co, &desc));
    CuAssertTrue(tc, NULL == co);
    CuAssertIntEquals(tc, 0, g_mco_allocs);
}
#endif

void test_minicoro(CuSuite *suite) {
#ifdef MCO_HAS_STACK_GUARD
    SUITE_ADD_TEST(suite, test_mco_stack_guard);
    SUITE_ADD_TEST(suite, test_mco_stack_guard_clean);
#endif
#ifdef TEST_MCO_FPU
    SUITE_ADD_TEST(suite, test_mco_fpu_isolated);
#endif
#if defined(MCO_USE_ASM) || defined(MCO_USE_UCONTEXT)
    SUITE_ADD_TEST(suite, test_mco_desc_overflow_rejected);
#endif
}
