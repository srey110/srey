#include "test_stm.h"
#include "utils/stm.h"
#include "base/macro.h"
#include "thread/thread.h"

// 多线程并发读 + 单 writer 持续 update; 验证无 race / 无泄漏 / 数据完整
#define _STM_CONC_READERS 4
#define _STM_CONC_ITERS   500
#define _STM_CONC_UPDATES 200

// 用 MALLOC 复制一段字符串数据, 出参 sz 含末尾 '\0'
static void *_stm_make(const char *s, size_t *out_sz) {
    size_t n = strlen(s) + 1;
    void *p;
    MALLOC(p, n);
    memcpy(p, s, n);
    *out_sz = n;
    return p;
}

// 基础: stm_new → stm_grab_data → stm_ungrab_data → stm_free 全路径无泄漏
static void test_stm_basic(CuTest *tc) {
    size_t sz;
    void *data = _stm_make("hello stm", &sz);
    stm_ctx *ctx = stm_new(data, sz, 0);
    stm_data *snap = stm_grab_data(ctx);
    CuAssertPtrNotNull(tc, snap);
    CuAssertIntEquals(tc, (int)sz, (int)snap->sz);
    CuAssertTrue(tc, 0 == memcmp(snap->data, "hello stm", sz));
    stm_ungrab_data(snap);
    stm_free(ctx);
}

// stm_grab_data_since: 快照没换时原样返回 last 且不加引用; 换了才拿到新快照(带引用); writer 释放后返回 NULL
static void test_stm_grab_since(CuTest *tc) {
    size_t sz;
    void *data = _stm_make("v1", &sz);
    stm_ctx *ctx = stm_new(data, sz, 0);
    stm_data *last = stm_grab_data(ctx);// writer 1 + 本 reader 1
    CuAssertIntEquals(tc, 2, (int32_t)ATOMIC_GET(&last->ref));
    CuAssertPtrEquals(tc, last, stm_grab_data_since(ctx, last));
    CuAssertIntEquals(tc, 2, (int32_t)ATOMIC_GET(&last->ref));
    stm_update(ctx, "v2", 3, 1);// 旧快照只剩本 reader 那一票
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&last->ref));
    stm_data *cur = stm_grab_data_since(ctx, last);
    CuAssertTrue(tc, cur != last);
    CuAssertTrue(tc, 0 == memcmp(cur->data, "v2", 3));
    CuAssertIntEquals(tc, 2, (int32_t)ATOMIC_GET(&cur->ref));
    stm_ungrab_data(last);
    // last 为 NULL 时退化成 stm_grab_data
    stm_data *again = stm_grab_data_since(ctx, NULL);
    CuAssertPtrEquals(tc, cur, again);
    CuAssertIntEquals(tc, 3, (int32_t)ATOMIC_GET(&cur->ref));
    stm_ungrab_data(again);
    stm_grab(ctx);
    stm_free(ctx);
    CuAssertPtrEquals(tc, NULL, stm_grab_data_since(ctx, cur));
    stm_ungrab_data(cur);
    stm_ungrab(ctx);
}
// update 后 stm_grab_data 拿到新快照指针; 旧快照仍可读到旧值
static void test_stm_update(CuTest *tc) {
    size_t sz1, sz2;
    void *d1 = _stm_make("ver1", &sz1);
    void *d2 = _stm_make("ver2", &sz2);
    stm_ctx *ctx = stm_new(d1, sz1, 0);
    stm_data *old = stm_grab_data(ctx);
    stm_update(ctx, d2, sz2, 0);
    // 旧快照仍可读 (reader 持有自己的引用, 不会被 update 中的 _stm_free_data 释放)
    CuAssertTrue(tc, 0 == memcmp(old->data, "ver1", sz1));
    // 新快照指针不同
    stm_data *cur = stm_grab_data(ctx);
    CuAssertTrue(tc, old != cur);
    CuAssertTrue(tc, 0 == memcmp(cur->data, "ver2", sz2));
    stm_ungrab_data(old);
    stm_ungrab_data(cur);
    stm_free(ctx);
}

// copy=1: 内部自己 MALLOC 一份, 调用方仍持有原缓冲。这是生产主路径——lpub_check_buf 对
// Lua 字符串一律强制 copy=1, 而其余用例全走 copy=0。两支对调时 snap->data == src 当场变红
static void test_stm_copy(CuTest *tc) {
    char src[] = "copied";
    char up[] = "copied2";
    stm_ctx *ctx = stm_new(src, sizeof(src), 1);
    stm_data *snap = stm_grab_data(ctx);
    CuAssertPtrNotNull(tc, snap);
    CuAssertTrue(tc, snap->data != src);// 另分配, 不是接管栈上那块
    CuAssertIntEquals(tc, (int)sizeof(src), (int)snap->sz);
    CuAssertTrue(tc, 0 == memcmp(snap->data, src, sizeof(src)));
    src[0] = 'X';// 改原缓冲不该影响已拍的快照
    CuAssertTrue(tc, 'c' == ((char *)snap->data)[0]);
    stm_ungrab_data(snap);
    stm_update(ctx, up, sizeof(up), 1);
    snap = stm_grab_data(ctx);
    CuAssertPtrNotNull(tc, snap);
    CuAssertTrue(tc, snap->data != up);
    CuAssertTrue(tc, 0 == memcmp(snap->data, up, sizeof(up)));
    stm_ungrab_data(snap);
    stm_free(ctx);
}

// writer 先释放, reader 后释放: ctx 在最后 reader 离开时 free
// stm_free 后 stm_grab_data 必须返回 NULL
static void test_stm_writer_first(CuTest *tc) {
    size_t sz;
    void *data = _stm_make("payload", &sz);
    stm_ctx *ctx = stm_new(data, sz, 0);
    stm_grab(ctx);// 模拟另一线程 reader 持引
    stm_data *snap = stm_grab_data(ctx);
    stm_free(ctx);
    // writer 退出后 ctx 仍存在 (reader 持引); 旧快照仍可读
    CuAssertTrue(tc, 0 == memcmp(snap->data, "payload", sz));
    // 再次 stm_grab_data 应得 NULL (ctx->data 已被 writer 清掉)
    stm_data *afternull = stm_grab_data(ctx);
    CuAssertPtrEquals(tc, NULL, afternull);
    stm_ungrab_data(snap);
    stm_ungrab(ctx);// 最后一个引用, 真正 free
}

// reader 先全部释放, writer 后释放: writer release 时 free
static void test_stm_reader_first(CuTest *tc) {
    size_t sz;
    void *data = _stm_make("payload", &sz);
    stm_ctx *ctx = stm_new(data, sz, 0);
    stm_grab(ctx);// reader 持引
    stm_data *snap = stm_grab_data(ctx);
    stm_ungrab_data(snap);
    stm_ungrab(ctx);
    // 此时 writer 仍持 ctx, 可继续 update; 验证 update 后能读到新版
    size_t sz2;
    void *d2 = _stm_make("v2", &sz2);
    stm_update(ctx, d2, sz2, 0);
    stm_data *snap2 = stm_grab_data(ctx);
    CuAssertTrue(tc, 0 == memcmp(snap2->data, "v2", sz2));
    stm_ungrab_data(snap2);
    stm_free(ctx);// 最后一个引用, free
}

// grab 链: grab N 次后 release N+1 次 (含 writer); ASan/内存检查通过即正确
static void test_stm_grab_chain(CuTest *tc) {
    size_t sz;
    void *data = _stm_make("chain", &sz);
    stm_ctx *ctx = stm_new(data, sz, 0);
    int i;
    for (i = 0; i < 5; i++) {
        stm_grab(ctx);
    }
    for (i = 0; i < 5; i++) {
        stm_ungrab(ctx);
    }
    // 五进五出之后引用计数必须回到初始的 1，否则 stm_free 要么提前放要么放不掉
    CuAssertIntEquals(tc, 1, (int32_t)ATOMIC_GET(&ctx->ref));
    stm_free(ctx);
}

typedef struct {
    stm_ctx *ctx;
    atomic_t reads;
    atomic_t mismatches;
} _stm_conc_shared;

static void _stm_conc_reader(void *arg) {
    _stm_conc_shared *s = (_stm_conc_shared *)arg;
    stm_grab(s->ctx);
    const char *p;
    int32_t bad;
    size_t k;
    int i;
    for (i = 0; i < _STM_CONC_ITERS; i++) {
        stm_data *snap = stm_grab_data(s->ctx);
        if (NULL != snap) {
            // 全量校验而非只看首字节: 所有版本都叫 "v<N>", 只看 [0] 的话读到已释放的旧快照
            // (例如去掉 stm_grab_data 的 rdlock) 通常仍读到 'v', release 下看不出来
            p = (const char *)snap->data;
            bad = (snap->sz < 2 || 'v' != p[0] || '\0' != p[snap->sz - 1]);
            for (k = 1; 0 == bad && k + 1 < snap->sz; k++) {
                if (p[k] < '0' || p[k] > '9') {
                    bad = 1;
                }
            }
            if (0 != bad) {
                ATOMIC_ADD(&s->mismatches, 1);
            }
            ATOMIC_ADD(&s->reads, 1);
            stm_ungrab_data(snap);
        }
    }
    stm_ungrab(s->ctx);
}

static void test_stm_concurrent_read(CuTest *tc) {
    size_t sz;
    void *data = _stm_make("v0", &sz);
    _stm_conc_shared s;
    s.ctx = stm_new(data, sz, 0);
    s.reads = 0;
    s.mismatches = 0;
    pthread_t ths[_STM_CONC_READERS];
    int i;
    for (i = 0; i < _STM_CONC_READERS; i++) {
        ths[i] = thread_creat(_stm_conc_reader, &s);
    }
    // 主线程作为 writer 持续 update
    char buf[16];
    size_t bz;
    for (i = 1; i <= _STM_CONC_UPDATES; i++) {
        SNPRINTF(buf, sizeof(buf), "v%d", i);
        void *bd = _stm_make(buf, &bz);
        stm_update(s.ctx, bd, bz, 0);
    }
    for (i = 0; i < _STM_CONC_READERS; i++) {
        thread_join(ths[i]);
    }
    CuAssertIntEquals(tc, 0, (int)ATOMIC_GET(&s.mismatches));
    /* 每个 reader 跑满 _STM_CONC_ITERS 轮，读次数是确定的；只判 >0 的话
       reader 提前退出、或 grab 失败被静默跳过都发现不了 */
    CuAssertIntEquals(tc, _STM_CONC_READERS * _STM_CONC_ITERS, (int)ATOMIC_GET(&s.reads));
    stm_free(s.ctx);
}

/* sz=0 且 data 为 NULL：上层放行这种入参，构造快照时不能拿 NULL 当 memcpy 源。
 * 同上，只在 Linux 的 UBSan 构建下变红；这里主要是把"(NULL, 0) 合法"钉进用例 */
static void test_stm_empty(CuTest *tc) {
    stm_ctx *ctx = stm_new(NULL, 0, 1);
    stm_data *snap = stm_grab_data(ctx);
    // 先收拾再断言：CuAssert 失败走 longjmp，夹在中间会漏掉 ctx 与快照，一次真失败还要多报一笔假泄漏
    int32_t has1 = (NULL != snap);
    size_t sz1 = has1 ? snap->sz : (size_t)-1;
    stm_ungrab_data(snap);
    /* update 走同一条构造路径 */
    stm_update(ctx, NULL, 0, 1);
    snap = stm_grab_data(ctx);
    int32_t has2 = (NULL != snap);
    size_t sz2 = has2 ? snap->sz : (size_t)-1;
    stm_ungrab_data(snap);
    stm_free(ctx);
    CuAssertTrue(tc, 0 != has1);
    CuAssertIntEquals(tc, 0, (int)sz1);
    CuAssertTrue(tc, 0 != has2);
    CuAssertIntEquals(tc, 0, (int)sz2);
}

/* ======================================================================= */

void test_stm(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_stm_basic);
    SUITE_ADD_TEST(suite, test_stm_update);
    SUITE_ADD_TEST(suite, test_stm_copy);
    SUITE_ADD_TEST(suite, test_stm_writer_first);
    SUITE_ADD_TEST(suite, test_stm_reader_first);
    SUITE_ADD_TEST(suite, test_stm_grab_chain);
    SUITE_ADD_TEST(suite, test_stm_concurrent_read);
    SUITE_ADD_TEST(suite, test_stm_empty);
    SUITE_ADD_TEST(suite, test_stm_grab_since);
}
