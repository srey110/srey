#include "test_stm.h"
#include "utils/stm.h"
#include "base/macro.h"
#include "thread/thread.h"

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

// writer 先释放, reader 后释放: ctx 在最后 reader 离开时 free
// stm_free 后 stm_grab_data 必须返回 NULL
static void test_stm_writer_first(CuTest *tc) {
    size_t sz;
    void *data = _stm_make("payload", &sz);
    stm_ctx *ctx = stm_new(data, sz, 0);
    stm_grab(ctx);  // 模拟另一线程 reader 持引
    stm_data *snap = stm_grab_data(ctx);
    stm_free(ctx);
    // writer 退出后 ctx 仍存在 (reader 持引); 旧快照仍可读
    CuAssertTrue(tc, 0 == memcmp(snap->data, "payload", sz));
    // 再次 stm_grab_data 应得 NULL (ctx->data 已被 writer 清掉)
    stm_data *afternull = stm_grab_data(ctx);
    CuAssertPtrEquals(tc, NULL, afternull);
    stm_ungrab_data(snap);
    stm_ungrab(ctx);  // 最后一个引用, 真正 free
}

// reader 先全部释放, writer 后释放: writer release 时 free
static void test_stm_reader_first(CuTest *tc) {
    size_t sz;
    void *data = _stm_make("payload", &sz);
    stm_ctx *ctx = stm_new(data, sz, 0);
    stm_grab(ctx);  // reader 持引
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
    stm_free(ctx);  // 最后一个引用, free
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
    stm_free(ctx);
    CuAssertTrue(tc, 1);
}

// 多线程并发读 + 单 writer 持续 update; 验证无 race / 无泄漏 / 数据完整
#define _STM_CONC_READERS 4
#define _STM_CONC_ITERS   500
#define _STM_CONC_UPDATES 200
typedef struct {
    stm_ctx *ctx;
    atomic_t reads;
    atomic_t mismatches;
} _stm_conc_shared;

static void _stm_conc_reader(void *arg) {
    _stm_conc_shared *s = (_stm_conc_shared *)arg;
    stm_grab(s->ctx);
    int i;
    for (i = 0; i < _STM_CONC_ITERS; i++) {
        stm_data *snap = stm_grab_data(s->ctx);
        if (NULL != snap) {
            // 任一版本均以 'v' 开头, 校验数据完整
            if (snap->sz < 1 || ((char *)snap->data)[0] != 'v') {
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
    for (i = 1; i <= _STM_CONC_UPDATES; i++) {
        SNPRINTF(buf, sizeof(buf), "v%d", i);
        size_t bz;
        void *bd = _stm_make(buf, &bz);
        stm_update(s.ctx, bd, bz, 0);
    }
    for (i = 0; i < _STM_CONC_READERS; i++) {
        thread_join(ths[i]);
    }
    CuAssertIntEquals(tc, 0, (int)ATOMIC_GET(&s.mismatches));
    CuAssertTrue(tc, ATOMIC_GET(&s.reads) > 0);
    stm_free(s.ctx);
}

/* ======================================================================= */

void test_stm(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_stm_basic);
    SUITE_ADD_TEST(suite, test_stm_update);
    SUITE_ADD_TEST(suite, test_stm_writer_first);
    SUITE_ADD_TEST(suite, test_stm_reader_first);
    SUITE_ADD_TEST(suite, test_stm_grab_chain);
    SUITE_ADD_TEST(suite, test_stm_concurrent_read);
}
