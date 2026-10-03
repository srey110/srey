#include "test_containers.h"
#include "test_rand.h"
#include "lib.h"

FSQU_DECL(i32_fsqu, int32_t)
FSQU_DECL(uptr_fsqu, uintptr_t)
MPQ_DECL(i32_mpq, int32_t)
MPQ_DECL(u32_mpq, uint32_t)
MPQ_DECL(uptr_mpq, uintptr_t)
BBQ_DECL(u32_bbq, uint32_t)
BBQ_DECL(uptr_bbq, uintptr_t)
SPSC_DECL(uptr_spsc, uintptr_t)
QUE_DECL(i32_que, int32_t)
QUE_DECL(i_que, int)
QUE_DECL(u32_que, uint32_t)
ARR_DECL(iarr, int)
ARR_DECL(parr, void *)
typedef struct { uint8_t b[200]; } _big200;
ARR_DECL(barr, _big200)
/* fsqu 的两个无锁环后端直接用本体实例化，不看 FSQU_FAST_MODEL：macOS / BSD 默认是 queue+spin，
   不这样的话 mpq / bbq 两个后端在那里一次都跑不到 */
FSQU_RING_DECL(i32_rfm, int32_t, MPQ_DECL)
FSQU_RING_DECL(i32_rfb, int32_t, BBQ_DECL)

#define _MPQ_MAXMS 30000 /* 兜底：mpq 真坏了要失败，不能挂死 */

/* mpq 并发用例共用的 producer / consumer 框架 */
#define _MPQ_PROD_CNT    4
#define _MPQ_CONS_CNT    4
#define _MPQ_ITEMS_EACH  50000
#define _MPQ_TOTAL       (_MPQ_PROD_CNT * _MPQ_ITEMS_EACH)

/* uptr_mpq_size 并发采样 */
#define _MPQ_SZ_CAP    4u
#define _MPQ_SZ_ROUNDS 100000

/* spsc 并发：1 生产者 × 1 消费者 */
#define _SPSC_ITEMS    200000

/* mpq 多生产多消费；容量压到 64 让抢占窗口够密 */
#define _MPQMP_PROD   4
#define _MPQMP_PER    25000
#define _MPQMP_TOTAL  (_MPQMP_PROD * _MPQMP_PER)
#define _MPQMP_CAP    64

// 队满后连打这么多次 trypush，全部必须立即失败。超时线是给"退避跑到满路径上"
// 这类回归用的：正常几毫秒，退避若卡在满路径上会是它的上千倍
#define _MPQ_FULL_HITS  200000
#define _MPQ_FULL_MAXMS 2000

/* fsqu 单生产者顺序：容量压到 4 让"环满即溢出"足够频繁 */
#define _FSQU_ORDER_TOTAL 5000000
#define _FSQU_ORDER_CAP   4

/* 批量出队 */
#define _MPQ_BATCH_WRAP 1000// 绕环轮数,每轮都让批量跨过环绕点
#define _MPQ_BATCH_MAX  32  // 单次批量上限,取 loader 的 TASK_MSG_BATCH

/* bbq 换代：最小容量下反复走完块环 */
#define _BBQ_WRAP_ROUNDS 200000

/* bbq 多生产者守恒；容量压到 64 让抢占窗口够密 */
#define _BBQMP_PROD   4
#define _BBQMP_PER    25000
#define _BBQMP_TOTAL  (_BBQMP_PROD * _BBQMP_PER)
#define _BBQMP_CAP    64

/* uptr_bbq_size 并发采样 */
#define _BBQ_SZ_CAP    64u
#define _BBQ_SZ_ROUNDS 20000

/* bbq 多消费者：生产者沿用 _BBQMP_* 那组 */
#define _BBQMC_CONS 3

/* chan 多生产多消费 */
#define _CHAN_RACE_CAP       4// 紧 buffer，强制 producer 等 consumer 取走再 push
#define _CHAN_RACE_PRODS     4
#define _CHAN_RACE_CONSS     4
#define _CHAN_RACE_PER_PROD  500
#define _CHAN_RACE_TOTAL     (_CHAN_RACE_PRODS * _CHAN_RACE_PER_PROD)

/* hashmap 上游不变式对拍 */
#define UP_N         256
#define UP_FAIL_ODDS 3

/* bbq 多生产者守恒用例的生产者参数 */
typedef struct bbqmp_arg {
    uint32_t base;      /* 本生产者的起始值，各段互不重叠 */
    u32_bbq *q;
}bbqmp_arg;
/* mpq 并发用例的生产者参数 */
typedef struct { uptr_mpq *q; int id; } _mpq_prod_arg;
typedef struct mpqmp_arg {
    uint32_t base;      /* 本生产者的起始值，各段互不重叠 */
    u32_mpq *q;
}mpqmp_arg;

/* chan 并发用例的消息与生产者参数 */
typedef struct _chan_race_msg {
    int32_t pid;
    int32_t seq;
    char padding[24];   // 32 字节，验证 chan 不串扰 padding
}_chan_race_msg;
typedef struct _chan_race_prod_arg {
    chan_ctx *chan;
    int32_t pid;
}_chan_race_prod_arg;

/* hashmap 通用用例的键值表 */
typedef struct { char key[32]; int val; } _kv;
#define _KV_HASH(e) hash((e)->key, strlen((e)->key))
#define _KV_CMP(a, b) strcmp((a)->key, (b)->key)
HASHMAP_DECL(kv_map, _kv, _KV_HASH, _KV_CMP)

/* 堆用例的节点：最小堆，LT 为真表示 lhs 优先于 rhs */
typedef struct { uint32_t hidx; int val; } _hnode;
#define _HNODE_LT(lhs, rhs) ((lhs)->val < (rhs)->val)
HEAP_DECL(_theap, _hnode, hidx, _HNODE_LT)

/* slist 用例的节点 */
typedef struct { list_node node; int val; } _lnode;

/* hashmap 上游自检移植用的整数表与字符串表 */
#define _UP_HASH_INT(e) hash_u64((uint64_t)*(e))
#define _UP_CMP_INT(a, b) ((*(a) > *(b)) - (*(a) < *(b)))
#define _UP_HASH_STR(e) hash(*(e), strlen(*(e)))
#define _UP_CMP_STR(a, b) strcmp(*(a), *(b))
HASHMAP_DECL(up_imap, int32_t, _UP_HASH_INT, _UP_CMP_INT)
HASHMAP_DECL(up_smap, char *, _UP_HASH_STR, _UP_CMP_STR)

/* rbtree 用例的元素，同一元素挂两棵树：bykey 允许重复键（MULTI），byid 唯一键（UNIQUE）。
   两个 rbt_node 都不放首位，覆盖 entry 对 NULL 的判断 */
typedef struct {
    int32_t key;
    uint32_t seq;// 插入序号，校验重复键按插入序排列
    rbt_node bykey;
    uint32_t id;
    int32_t pad;
    rbt_node byid;
} _rbe;
#define _RB_LT(a, b) ((a) < (b))
#define _RBE_KEY(e) ((e)->key)
#define _RBE_ID(e) ((e)->id)
RBT_DECL(_rbk, _rbe, bykey, int32_t, _RBE_KEY, _RB_LT, RBT_MULTI)
RBT_DECL(_rbi, _rbe, byid, uint32_t, _RBE_ID, _RB_LT, RBT_UNIQUE)

/* rbtree 正常用法示例用的元素：一个"任务"同时挂三棵树 —— 按名字查（字符串键）、按分数排（组合键）、
   按到期时间排（定时器队列：底层接口手写下降循环 + 最左缓存，同一时刻允许多个） */
typedef struct {
    uint32_t id;
    int32_t fired;
    const char *name;
    int64_t score;
    uint64_t due;
    rbt_node byname;
    rbt_node byscore;
    rbt_node bydue;
} _rbu;
/* 分数相同再比 id，组合键唯一；按分数查区间就用 lower_bound({分数, 0}) */
typedef struct {
    int64_t score;
    uint32_t id;
} _rbu_sk;
#define _RBU_NAME(e) ((e)->name)
#define _RBU_STR_LT(a, b) (strcmp((a), (b)) < 0)
RBT_DECL(_rbu_name, _rbu, byname, const char *, _RBU_NAME, _RBU_STR_LT, RBT_UNIQUE)
#define _RBU_SK(e) ((_rbu_sk){ (e)->score, (e)->id })
#define _RBU_SK_LT(a, b) ((a).score < (b).score || ((a).score == (b).score && (a).id < (b).id))
RBT_DECL(_rbu_score, _rbu, byscore, _rbu_sk, _RBU_SK, _RBU_SK_LT, RBT_UNIQUE)

/* rbtset 用例的元素：key 参与比较，val 不参与（区分覆盖前后、校验重复键的插入序） */
typedef struct {
    int32_t key;
    int32_t val;
} _rse;
#define _RSE_KEY(e) ((e)->key)
RBTSET_DECL(_rsu, _rse, int32_t, _RSE_KEY, _RB_LT, RBT_UNIQUE)
RBTSET_DECL(_rsm, _rse, int32_t, _RSE_KEY, _RB_LT, RBT_MULTI)
/* 持有堆内存的元素：校验只有 free 调 elfree，覆盖与删除交出的副本归调用方 */
typedef struct {
    int32_t key;
    char *name;
} _rss;
RBTSET_DECL(_rss_set, _rss, int32_t, _RSE_KEY, _RB_LT, RBT_UNIQUE)
#define _RSS_NAME_LEN 16

/* 宏生成的用例里断言行号都落在展开那一行，靠把条件原文放进失败信息来定位 */
#define _FQ_ASSERT(tc, cond) CuAssert(tc, #cond, cond)

/* 单生产者按序推、单消费者批量取（loader 取 task 消息就是这个形态），序号必须严格递增。
   快路径是无锁环时，消费者判完"环空"到去取溢出层之间，生产者可能已把环填满再溢出，
   锁内不重查环的话，溢出层里更新的元素会插到环里更早的元素前面；条数少了抓不稳，故取 _FSQU_ORDER_TOTAL。
   queue+spin 后端没有溢出层，这条必然通过 */
#define _FSQU_ORDER_CHECK(Q)                                                   \
static void Q##_order_producer(void *arg) {                                     \
    Q *q = (Q *)arg;                                                            \
    int32_t v;                                                                  \
    for (v = 1; v <= _FSQU_ORDER_TOTAL; v++) {                                  \
        Q##_push(q, &v);                                                        \
    }                                                                           \
}                                                                               \
static void Q##_order_check(CuTest *tc) {                                       \
    Q q;                                                                        \
    int32_t batch[8];                                                           \
    int32_t last = 0;                                                           \
    uint32_t i, n, got = 0, bad = 0, fails = 0;                                 \
    uint64_t deadline;                                                          \
    pthread_t th;                                                               \
    Q##_init(&q, _FSQU_ORDER_CAP);                                              \
    th = thread_creat(Q##_order_producer, &q);                                  \
    deadline = nowms() + _MPQ_MAXMS;                                            \
    while (got < _FSQU_ORDER_TOTAL) {                                           \
        n = Q##_pop_sc_batch(&q, batch, 8);                                     \
        for (i = 0; i < n; i++) {                                               \
            if (batch[i] != last + 1) {                                         \
                bad++;                                                          \
            }                                                                   \
            last = batch[i];                                                    \
            got++;                                                              \
        }                                                                       \
        if (0 != n) {                                                           \
            continue;                                                           \
        }                                                                       \
        /* 生产者 push 永不阻塞，超时直接 break 也能 join 回来，不需要 stop 标志 */ \
        if (0 == (++fails & 0xFFFFF)                                            \
            && nowms() > deadline) {                                            \
            break;                                                              \
        }                                                                       \
    }                                                                           \
    thread_join(th);                                                            \
    Q##_free(&q);                                                               \
    CuAssertIntEquals_Msg(tc, #Q " out of order", 0, (int32_t)bad);             \
    CuAssertIntEquals_Msg(tc, #Q " lost", _FSQU_ORDER_TOTAL, (int32_t)got);     \
}

/* 无锁环后端 fsqu 的核心断言：trypush 满即拒且不落溢出层、FIFO 跨快路径与溢出层、
   粘滞降级（溢出层非空期间 push 落溢出层、trypush 一律拒）、size/empty 含溢出层、批量跨界取齐。
   sc 选出队族：bbq 上 pop 与 pop_sc 不能在同一队列混用，两族各用一个新队列走一遍 */
#define _FSQU_RING_CHECK(Q)                                                    \
static int32_t Q##_pop_mode(Q *q, int32_t *out, int32_t sc) {                   \
    return sc ? Q##_pop_sc(q, out) : Q##_pop(q, out);                           \
}                                                                               \
static uint32_t Q##_batch_mode(Q *q, int32_t *out, uint32_t max, int32_t sc) {  \
    return sc ? Q##_pop_sc_batch(q, out, max) : Q##_pop_batch(q, out, max);     \
}                                                                               \
static void Q##_core_check(CuTest *tc, int32_t sc) {                            \
    Q q;                                                                        \
    int32_t v, out, buf[12];                                                    \
    uint32_t i, n;                                                              \
    Q##_init(&q, 8);                                                            \
    _FQ_ASSERT(tc, 8 == Q##_capacity(&q));                                      \
    _FQ_ASSERT(tc, sizeof(int32_t) == Q##_elsize(&q));                          \
    _FQ_ASSERT(tc, 0 == Q##_size(&q) && Q##_empty(&q));                         \
    _FQ_ASSERT(tc, ERR_FAILED == Q##_pop_mode(&q, &out, sc));                   \
    /* trypush 装满即拒，且不落溢出层 */                                         \
    for (v = 1; v <= 8; v++) {                                                  \
        _FQ_ASSERT(tc, ERR_OK == Q##_trypush(&q, &v));                          \
    }                                                                           \
    _FQ_ASSERT(tc, ERR_FAILED == Q##_trypush(&q, &v));                          \
    _FQ_ASSERT(tc, 8 == Q##_size(&q) && !Q##_empty(&q));                        \
    for (v = 1; v <= 8; v++) {                                                  \
        _FQ_ASSERT(tc, ERR_OK == Q##_pop_mode(&q, &out, sc) && out == v);       \
    }                                                                           \
    _FQ_ASSERT(tc, ERR_FAILED == Q##_pop_mode(&q, &out, sc));                   \
    /* 1..8 进快路径，9、10 落溢出层；腾出 3 个后 11 仍须落溢出层、trypush 须拒 */  \
    for (v = 1; v <= 10; v++) {                                                 \
        Q##_push(&q, &v);                                                       \
    }                                                                           \
    _FQ_ASSERT(tc, 10 == Q##_size(&q) && !Q##_empty(&q));                       \
    for (v = 1; v <= 3; v++) {                                                  \
        _FQ_ASSERT(tc, ERR_OK == Q##_pop_mode(&q, &out, sc) && out == v);       \
    }                                                                           \
    v = 11;                                                                     \
    Q##_push(&q, &v);                                                           \
    v = 999;                                                                    \
    _FQ_ASSERT(tc, ERR_FAILED == Q##_trypush(&q, &v));                          \
    _FQ_ASSERT(tc, 8 == Q##_size(&q));                                          \
    /* 一次批量跨快路径与溢出层取齐 4..11 */                                     \
    n = Q##_batch_mode(&q, buf, 12, sc);                                        \
    _FQ_ASSERT(tc, 8 == n);                                                     \
    for (i = 0; i < n; i++) {                                                   \
        _FQ_ASSERT(tc, (int32_t)i + 4 == buf[i]);                               \
    }                                                                           \
    _FQ_ASSERT(tc, 0 == Q##_size(&q) && Q##_empty(&q));                         \
    _FQ_ASSERT(tc, ERR_FAILED == Q##_pop_mode(&q, &out, sc));                   \
    /* push_batch 超出快路径的余量整批落溢出层；先批量、再单条跨进溢出层，顺序不乱 */ \
    for (i = 0; i < 12; i++) {                                                  \
        buf[i] = (int32_t)i + 1;                                                \
    }                                                                           \
    Q##_push_batch(&q, buf, 12);                                                \
    _FQ_ASSERT(tc, 12 == Q##_size(&q));                                         \
    n = Q##_batch_mode(&q, buf, 5, sc);                                         \
    _FQ_ASSERT(tc, 5 == n && 1 == buf[0] && 5 == buf[4]);                       \
    _FQ_ASSERT(tc, ERR_OK == Q##_pop_mode(&q, &out, sc) && 6 == out);           \
    /* 余下 7..12 逐条取：快路径取空后，单条出队要能接着从溢出层取 */           \
    for (v = 7; v <= 12; v++) {                                                 \
        _FQ_ASSERT(tc, ERR_OK == Q##_pop_mode(&q, &out, sc) && out == v);       \
    }                                                                           \
    _FQ_ASSERT(tc, 0 == Q##_batch_mode(&q, buf, 12, sc) && Q##_empty(&q));      \
    /* 溢出层排空后粘滞解除，trypush 又能装满快路径 */                             \
    for (v = 1; v <= 8; v++) {                                                  \
        _FQ_ASSERT(tc, ERR_OK == Q##_trypush(&q, &v));                          \
    }                                                                           \
    _FQ_ASSERT(tc, ERR_FAILED == Q##_trypush(&q, &v));                          \
    for (v = 1; v <= 8; v++) {                                                  \
        _FQ_ASSERT(tc, ERR_OK == Q##_pop_mode(&q, &out, sc) && out == v);       \
    }                                                                           \
    _FQ_ASSERT(tc, 0 == Q##_size(&q) && Q##_empty(&q));                         \
    Q##_free(&q);                                                               \
}

_FSQU_ORDER_CHECK(i32_fsqu)
_FSQU_ORDER_CHECK(i32_rfm)
_FSQU_ORDER_CHECK(i32_rfb)
_FSQU_RING_CHECK(i32_rfm)
_FSQU_RING_CHECK(i32_rfb)

/* =======================================================================
 * mpq —— 无锁多生产者有界队列（消费者侧两种 API：uptr_mpq_pop 多消费者 / uptr_mpq_pop_sc 单消费者）
 * ======================================================================= */

/* 单线程：基本入队出队、FIFO 顺序（uptr_mpq_pop 路径） */
static void test_mpq_basic(CuTest *tc) {
    uptr_mpq q;
    uintptr_t v, out;
    uptr_mpq_init(&q, 0);/* 0 → 默认容量 1024 */

    CuAssertTrue(tc, 1024 == uptr_mpq_capacity(&q));
    CuAssertTrue(tc, 0 == uptr_mpq_size(&q));
    CuAssertTrue(tc, ERR_FAILED == uptr_mpq_pop(&q, &out));/* 空队列出队返回 ERR_FAILED */

    /* 入队 10 个整数值 */
    for (uintptr_t i = 1; i <= 10; i++) {
        v = i;
        CuAssertTrue(tc, ERR_OK == uptr_mpq_trypush(&q, &v));
    }
    CuAssertTrue(tc, 10 == uptr_mpq_size(&q));

    /* FIFO 顺序验证 */
    for (uintptr_t i = 1; i <= 10; i++) {
        CuAssertTrue(tc, ERR_OK == uptr_mpq_pop(&q, &out) && out == i);
    }
    CuAssertTrue(tc, 0 == uptr_mpq_size(&q));
    CuAssertTrue(tc, ERR_FAILED == uptr_mpq_pop(&q, &out));
    uptr_mpq_free(&q);
}

/* 单线程：基本入队出队、FIFO 顺序（uptr_mpq_pop_sc 路径，独立验证单消费者算法） */
static void test_mpq_basic_sc(CuTest *tc) {
    uptr_mpq q;
    uintptr_t v, out;
    uptr_mpq_init(&q, 0);

    CuAssertTrue(tc, ERR_FAILED == uptr_mpq_pop_sc(&q, &out));/* 空队列出队返回 ERR_FAILED */

    for (uintptr_t i = 1; i <= 10; i++) {
        v = i;
        CuAssertTrue(tc, ERR_OK == uptr_mpq_trypush(&q, &v));
    }
    for (uintptr_t i = 1; i <= 10; i++) {
        CuAssertTrue(tc, ERR_OK == uptr_mpq_pop_sc(&q, &out) && out == i);
    }
    CuAssertTrue(tc, 0 == uptr_mpq_size(&q));
    CuAssertTrue(tc, ERR_FAILED == uptr_mpq_pop_sc(&q, &out));
    uptr_mpq_free(&q);
}

/* 边界：队列填满后拒绝入队；非 2 的幂容量自动向上对齐 */
static void test_mpq_boundary(CuTest *tc) {
    uptr_mpq q;
    uintptr_t v, out;

    /* 容量 4，填满后 push 返回 ERR_FAILED */
    uptr_mpq_init(&q, 4);
    CuAssertTrue(tc, 4 == uptr_mpq_capacity(&q));
    for (uintptr_t i = 1; i <= 4; i++) {
        v = i;
        CuAssertTrue(tc, ERR_OK == uptr_mpq_trypush(&q, &v));
    }
    v = 5;
    CuAssertTrue(tc, ERR_FAILED == uptr_mpq_trypush(&q, &v));

    /* 消费 2 个后可再入队 2 个 */
    CuAssertTrue(tc, ERR_OK == uptr_mpq_pop(&q, &out) && 1 == out);
    CuAssertTrue(tc, ERR_OK == uptr_mpq_pop(&q, &out) && 2 == out);
    v = 5; CuAssertTrue(tc, ERR_OK == uptr_mpq_trypush(&q, &v));
    v = 6; CuAssertTrue(tc, ERR_OK == uptr_mpq_trypush(&q, &v));
    v = 7; CuAssertTrue(tc, ERR_FAILED == uptr_mpq_trypush(&q, &v));

    /* 出队顺序 */
    CuAssertTrue(tc, ERR_OK == uptr_mpq_pop(&q, &out) && 3 == out);
    CuAssertTrue(tc, ERR_OK == uptr_mpq_pop(&q, &out) && 4 == out);
    CuAssertTrue(tc, ERR_OK == uptr_mpq_pop(&q, &out) && 5 == out);
    CuAssertTrue(tc, ERR_OK == uptr_mpq_pop(&q, &out) && 6 == out);
    CuAssertTrue(tc, ERR_FAILED == uptr_mpq_pop(&q, &out));
    uptr_mpq_free(&q);

    /* 容量 5（非 2 的幂）→ 自动对齐为 8 */
    uptr_mpq_init(&q, 5);
    CuAssertTrue(tc, 8 == uptr_mpq_capacity(&q));
    for (uintptr_t i = 1; i <= 8; i++) {
        v = i;
        CuAssertTrue(tc, ERR_OK == uptr_mpq_trypush(&q, &v));
    }
    v = 9;
    CuAssertTrue(tc, ERR_FAILED == uptr_mpq_trypush(&q, &v));
    uptr_mpq_free(&q);
}

static atomic_t _mpq_stop;

static void _mpq_producer(void *arg) {
    _mpq_prod_arg *a = (_mpq_prod_arg *)arg;
    uintptr_t start = (uintptr_t)a->id * _MPQ_ITEMS_EACH + 1;
    uintptr_t end   = start + _MPQ_ITEMS_EACH;
    uintptr_t v;
    for (v = start; v < end; v++) {
        // mpq 只提供非阻塞入队,满则自旋重试(测试线程非消费者,不会自死锁)
        while (ERR_OK != uptr_mpq_trypush(a->q, &v)) {
            // 消费者超时会置 stop；不给这条出路的话满队自旋会让 join 永远回不来
            if (0 != ATOMIC_GET(&_mpq_stop)) {
                return;
            }
            CPU_PAUSE();
        }
    }
}

static atomic_t _mpq_consumed;
static atomic64_t _mpq_sum;

/* mpq 消费者：use_sc=0 走 uptr_mpq_pop（多消费者 CAS 路径），=1 走 uptr_mpq_pop_sc（单消费者无 CAS）。
   两条路径除这一句取元素外逐字相同，合成一个函数免得改超时/退出条件时漏改一边 */
static void _mpq_consume_loop(uptr_mpq *q, int32_t use_sc) {
    uintptr_t p;
    uint32_t prev;
    uint32_t fails = 0;
    uint64_t deadline = nowms() + _MPQ_MAXMS;
    for (;;) {
        if (ERR_OK == (use_sc ? uptr_mpq_pop_sc(q, &p) : uptr_mpq_pop(q, &p))) {
            prev = ATOMIC_ADD(&_mpq_consumed, 1);
            ATOMIC64_ADD(&_mpq_sum, p);
            if (prev + 1 >= (uint32_t)_MPQ_TOTAL) {
                break;
            }
        } else {
            if (ATOMIC_GET(&_mpq_consumed) >= (uint32_t)_MPQ_TOTAL) {
                break;
            }
            // 每约 100 万次空转才看一次表：取不到时是紧循环,逐次 nowms 会拖慢整个用例。
            // 到点置 stop 让生产者也能退出
            if (0 == (++fails & 0xFFFFF)
                && nowms() > deadline) {
                ATOMIC_SET(&_mpq_stop, 1);
                break;
            }
            CPU_PAUSE();
        }
    }
}
static void _mpq_consumer_mc(void *arg) {
    _mpq_consume_loop((uptr_mpq *)arg, 0);
}
static void _mpq_consumer_sc(void *arg) {
    _mpq_consume_loop((uptr_mpq *)arg, 1);
}

/* 并发：4 生产者 × 4 消费者（uptr_mpq_pop），验证无丢失、无重复 */
static void test_mpq_concurrent_mc(CuTest *tc) {
    uptr_mpq q;
    uptr_mpq_init(&q, 1024);

    _mpq_consumed = 0;
    _mpq_sum      = 0;
    ATOMIC_SET(&_mpq_stop, 0);
    int64_t expected = (int64_t)_MPQ_TOTAL * (_MPQ_TOTAL + 1) / 2;

    pthread_t producers[_MPQ_PROD_CNT];
    pthread_t consumers[_MPQ_CONS_CNT];
    _mpq_prod_arg pargs[_MPQ_PROD_CNT];
    int i;

    /* 先启动消费者，避免生产者长时间自旋 */
    for (i = 0; i < _MPQ_CONS_CNT; i++) {
        consumers[i] = thread_creat(_mpq_consumer_mc, &q);
    }
    for (i = 0; i < _MPQ_PROD_CNT; i++) {
        pargs[i].q  = &q;
        pargs[i].id = i;
        producers[i] = thread_creat(_mpq_producer, &pargs[i]);
    }
    for (i = 0; i < _MPQ_PROD_CNT; i++) {
        thread_join(producers[i]);
    }
    for (i = 0; i < _MPQ_CONS_CNT; i++) {
        thread_join(consumers[i]);
    }

    CuAssertTrue(tc, (uint32_t)_MPQ_TOTAL == ATOMIC_GET(&_mpq_consumed));
    CuAssertTrue(tc, expected == (int64_t)ATOMIC64_GET(&_mpq_sum));
    uptr_mpq_free(&q);
}

/* 并发：4 生产者 × 1 消费者（uptr_mpq_pop_sc），验证无丢失、无重复 */
static void test_mpq_concurrent_sc(CuTest *tc) {
    uptr_mpq q;
    uptr_mpq_init(&q, 1024);

    _mpq_consumed = 0;
    _mpq_sum      = 0;
    ATOMIC_SET(&_mpq_stop, 0);
    int64_t expected = (int64_t)_MPQ_TOTAL * (_MPQ_TOTAL + 1) / 2;

    pthread_t producers[_MPQ_PROD_CNT];
    pthread_t consumer;
    _mpq_prod_arg pargs[_MPQ_PROD_CNT];
    int i;

    consumer = thread_creat(_mpq_consumer_sc, &q);
    for (i = 0; i < _MPQ_PROD_CNT; i++) {
        pargs[i].q  = &q;
        pargs[i].id = i;
        producers[i] = thread_creat(_mpq_producer, &pargs[i]);
    }
    for (i = 0; i < _MPQ_PROD_CNT; i++) {
        thread_join(producers[i]);
    }
    thread_join(consumer);

    CuAssertTrue(tc, (uint32_t)_MPQ_TOTAL == ATOMIC_GET(&_mpq_consumed));
    CuAssertTrue(tc, expected == (int64_t)ATOMIC64_GET(&_mpq_sum));
    uptr_mpq_free(&q);
}

/* uptr_mpq_size 只高估不低估：队列恒非空时不得读出 0。
 * churn 线程做 pop_sc → trypush 的往返，元素数只在两步之间降到 capacity-1，
 * 恒 >= 1；watch 线程并发采样 uptr_mpq_size()。
 * 修复前 uptr_mpq_size 先读 enq 后读 deq，得到的是 enq(旧) - deq(新)，系统性低估：
 * 两次读取之间队列被消费再填满，就会把非空队列报成 0。日志线程(log.c:130/136)与
 * worker(loader.c:247) 正是拿这个值判断"是否还有活要干"，报 0 即漏唤醒。
 * 改为先读 deq 后读 enq 之后 enq(新) - deq(旧) >= 真实值，本断言恒成立不会偶发。
 * 注：作为旧 bug 的检测器强度依赖 watch 线程恰在两次载入之间被抢占，
 * 但作为新契约的守卫是确定性的 */
static atomic_t _mpq_sz_zero;// 采样到 0 的次数，应恒为 0
static atomic_t _mpq_sz_stop;
static atomic_t _mpq_sz_fail;// churn 入队重试超时置 1
static void _mpq_size_churn(void *arg) {
    uptr_mpq *q = (uptr_mpq *)arg;
    uintptr_t p;
    int32_t i;
    uint32_t spins = 0;
    uint64_t deadline = nowms() + _MPQ_MAXMS;
    for (i = 0; i < _MPQ_SZ_ROUNDS; i++) {
        if (ERR_OK == uptr_mpq_pop_sc(q, &p)) {
            while (ERR_OK != uptr_mpq_trypush(q, &p)) {
                // 槽位迟迟放不回去就判失败退出，不然 churn 与 watch 一起挂死、join 回不来
                if (0 == (++spins & 0xFFFFF)
                    && nowms() > deadline) {
                    ATOMIC_SET(&_mpq_sz_fail, 1);
                    ATOMIC_SET(&_mpq_sz_stop, 1);
                    return;
                }
                CPU_PAUSE();
            }
        }
    }
    ATOMIC_SET(&_mpq_sz_stop, 1);
}
static void _mpq_size_watch(void *arg) {
    uptr_mpq *q = (uptr_mpq *)arg;
    while (0 == ATOMIC_GET(&_mpq_sz_stop)) {
        if (0 == uptr_mpq_size(q)) {
            ATOMIC_ADD(&_mpq_sz_zero, 1);
        }
        // 纯热转会把 churn 线程要读写的那几条缓存行占死, 单核上更是直接饿着它;
        // 采样密度掉一点无所谓, churn 的轮次是固定的, 样本量仍然够
        CPU_PAUSE();
    }
}
static void test_mpq_size_never_underreports(CuTest *tc) {
    uptr_mpq q;
    pthread_t churn, watch;
    uintptr_t v;
    uint32_t i;

    uptr_mpq_init(&q, _MPQ_SZ_CAP);
    // 先灌满：此后元素数只在 pop 与 push 之间短暂降到 capacity-1(>=1)
    for (i = 0; i < _MPQ_SZ_CAP; i++) {
        v = (uintptr_t)(i + 1);
        CuAssertTrue(tc, ERR_OK == uptr_mpq_trypush(&q, &v));
    }
    CuAssertTrue(tc, _MPQ_SZ_CAP == uptr_mpq_size(&q));

    ATOMIC_SET(&_mpq_sz_zero, 0);
    ATOMIC_SET(&_mpq_sz_stop, 0);
    ATOMIC_SET(&_mpq_sz_fail, 0);
    watch = thread_creat(_mpq_size_watch, &q);
    churn = thread_creat(_mpq_size_churn, &q);
    thread_join(churn);
    thread_join(watch);

    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_mpq_sz_fail));
    // 往返是配平的，跑完应回到满
    CuAssertTrue(tc, _MPQ_SZ_CAP == uptr_mpq_size(&q));
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_mpq_sz_zero));
    uptr_mpq_free(&q);
}

/* =======================================================================
 * spsc —— 无锁单生产者单消费者有界队列
 * ======================================================================= */

/* 单线程：基本入队出队、FIFO 顺序 */
static void test_spsc_basic(CuTest *tc) {
    uptr_spsc q;
    uintptr_t v, out;
    uptr_spsc_init(&q, 0);/* 0 → 默认容量 1024 */

    CuAssertTrue(tc, 1024 == uptr_spsc_capacity(&q));
    CuAssertTrue(tc, 0 == uptr_spsc_size(&q));
    CuAssertTrue(tc, ERR_FAILED == uptr_spsc_pop(&q, &out));

    for (uintptr_t i = 1; i <= 10; i++) {
        v = i;
        CuAssertTrue(tc, ERR_OK == uptr_spsc_trypush(&q, &v));
    }
    CuAssertTrue(tc, 10 == uptr_spsc_size(&q));

    for (uintptr_t i = 1; i <= 10; i++) {
        CuAssertTrue(tc, ERR_OK == uptr_spsc_pop(&q, &out) && out == i);
    }
    CuAssertTrue(tc, 0 == uptr_spsc_size(&q));
    CuAssertTrue(tc, ERR_FAILED == uptr_spsc_pop(&q, &out));
    uptr_spsc_free(&q);
}

/* 边界：队列填满后拒绝入队；非 2 的幂容量自动向上对齐 */
static void test_spsc_boundary(CuTest *tc) {
    uptr_spsc q;
    uintptr_t v, out;

    uptr_spsc_init(&q, 4);
    CuAssertTrue(tc, 4 == uptr_spsc_capacity(&q));
    for (uintptr_t i = 1; i <= 4; i++) {
        v = i;
        CuAssertTrue(tc, ERR_OK == uptr_spsc_trypush(&q, &v));
    }
    v = 5;
    CuAssertTrue(tc, ERR_FAILED == uptr_spsc_trypush(&q, &v));

    CuAssertTrue(tc, ERR_OK == uptr_spsc_pop(&q, &out) && 1 == out);
    CuAssertTrue(tc, ERR_OK == uptr_spsc_pop(&q, &out) && 2 == out);
    v = 5; CuAssertTrue(tc, ERR_OK == uptr_spsc_trypush(&q, &v));
    v = 6; CuAssertTrue(tc, ERR_OK == uptr_spsc_trypush(&q, &v));
    v = 7; CuAssertTrue(tc, ERR_FAILED == uptr_spsc_trypush(&q, &v));

    CuAssertTrue(tc, ERR_OK == uptr_spsc_pop(&q, &out) && 3 == out);
    CuAssertTrue(tc, ERR_OK == uptr_spsc_pop(&q, &out) && 4 == out);
    CuAssertTrue(tc, ERR_OK == uptr_spsc_pop(&q, &out) && 5 == out);
    CuAssertTrue(tc, ERR_OK == uptr_spsc_pop(&q, &out) && 6 == out);
    CuAssertTrue(tc, ERR_FAILED == uptr_spsc_pop(&q, &out));
    uptr_spsc_free(&q);

    /* 容量 5（非 2 的幂）→ 自动对齐为 8 */
    uptr_spsc_init(&q, 5);
    CuAssertTrue(tc, 8 == uptr_spsc_capacity(&q));
    for (uintptr_t i = 1; i <= 8; i++) {
        v = i;
        CuAssertTrue(tc, ERR_OK == uptr_spsc_trypush(&q, &v));
    }
    v = 9;
    CuAssertTrue(tc, ERR_FAILED == uptr_spsc_trypush(&q, &v));
    uptr_spsc_free(&q);
}

static atomic_t _spsc_done_prod;
static atomic_t _spsc_stop;

static void _spsc_producer(void *arg) {
    uptr_spsc *q = (uptr_spsc *)arg;
    uintptr_t v;
    for (v = 1; v <= _SPSC_ITEMS; v++) {
        // spsc 只提供非阻塞入队,满则自旋重试(测试线程非消费者,不会自死锁)
        while (ERR_OK != uptr_spsc_trypush(q, &v)) {
            // 消费者超时会置 stop；不给这条出路的话满队自旋会让 join 永远回不来
            if (0 != ATOMIC_GET(&_spsc_stop)) {
                return;
            }
            CPU_PAUSE();
        }
    }
    ATOMIC_SET(&_spsc_done_prod, 1);
}

static atomic_t _spsc_fail;/* 顺序违例计数 */
static atomic_t _spsc_consumed;

static void _spsc_consumer(void *arg) {
    uptr_spsc *q = (uptr_spsc *)arg;
    uintptr_t p;
    uintptr_t expected = 1;
    uint32_t fails = 0;
    const uint64_t maxms = 30000;/* 兜底：spsc 真坏了要失败，不能挂死 */
    uint64_t deadline = nowms() + maxms;
    for (;;) {
        if (ERR_OK == uptr_spsc_pop(q, &p)) {
            if (p != expected) {
                ATOMIC_ADD(&_spsc_fail, 1);
            }
            expected++;
            uint32_t cnt = ATOMIC_ADD(&_spsc_consumed, 1) + 1;
            if (cnt >= (uint32_t)_SPSC_ITEMS) {
                break;
            }
        } else {
            if (ATOMIC_GET(&_spsc_done_prod)
                && ATOMIC_GET(&_spsc_consumed) >= (uint32_t)_SPSC_ITEMS) {
                break;
            }
            // 每约 100 万次空转才看一次表：取不到时是紧循环,逐次 nowms 会拖慢整个用例
            if (0 == (++fails & 0xFFFFF)
                && nowms() > deadline) {
                ATOMIC_SET(&_spsc_stop, 1);
                break;
            }
            CPU_PAUSE();
        }
    }
}

static void test_spsc_concurrent(CuTest *tc) {
    uptr_spsc q;
    uptr_spsc_init(&q, 1024);

    ATOMIC_SET(&_spsc_done_prod, 0);
    ATOMIC_SET(&_spsc_fail, 0);
    ATOMIC_SET(&_spsc_consumed, 0);
    ATOMIC_SET(&_spsc_stop, 0);

    pthread_t cons = thread_creat(_spsc_consumer, &q);
    pthread_t prod = thread_creat(_spsc_producer, &q);
    thread_join(prod);
    thread_join(cons);

    CuAssertTrue(tc, (uint32_t)_SPSC_ITEMS == ATOMIC_GET(&_spsc_consumed));
    CuAssertTrue(tc, 0 == ATOMIC_GET(&_spsc_fail));/* FIFO 顺序严格成立 */
    uptr_spsc_free(&q);
}

/* =======================================================================
 * fsqu —— 平台自适应队列（FSQU_FAST_MODEL 选快路径：queue+spin / mpq / bbq）
 * 用 capacity 8（2 的幂）使各后端的 i32_fsqu_capacity 都返回 8。
 * ======================================================================= */

/* 基本：init 后 size==0、capacity==8；push/pop 往返 FIFO 顺序与值一致；空队列 pop 返回 ERR_FAILED */
static void test_fsqu_basic(CuTest *tc) {
    i32_fsqu q;
    int32_t v, out;
    i32_fsqu_init(&q, 8);

    CuAssertTrue(tc, 0 == i32_fsqu_size(&q));
    CuAssertTrue(tc, 8 == i32_fsqu_capacity(&q));
    CuAssertTrue(tc, ERR_FAILED == i32_fsqu_pop(&q, &out));/* 空队列出队 */

    /* push 8 个，size 随之递增 */
    for (v = 1; v <= 8; v++) {
        i32_fsqu_push(&q, &v);
        CuAssertTrue(tc, (uint32_t)v == i32_fsqu_size(&q));
    }

    /* FIFO 顺序出队，值一致，size 随之递减 */
    for (v = 1; v <= 8; v++) {
        CuAssertTrue(tc, ERR_OK == i32_fsqu_pop(&q, &out) && out == v);
        CuAssertTrue(tc, (uint32_t)(8 - v) == i32_fsqu_size(&q));
    }
    CuAssertTrue(tc, 0 == i32_fsqu_size(&q));
    CuAssertTrue(tc, ERR_FAILED == i32_fsqu_pop(&q, &out));/* 取空后再 pop */
    i32_fsqu_free(&q);
}

/* trypush：填满到 capacity 后再 trypush 返回 ERR_FAILED；消费一个后又可入队一个 */
static void test_fsqu_trypush_full(CuTest *tc) {
    i32_fsqu q;
    int32_t v, out;
    uint32_t grain = 0, k;
    i32_fsqu_init(&q, 8);

    for (v = 1; v <= 8; v++) {
        CuAssertTrue(tc, ERR_OK == i32_fsqu_trypush(&q, &v));
    }
    CuAssertTrue(tc, 8 == i32_fsqu_size(&q));
    v = 9;
    CuAssertTrue(tc, ERR_FAILED == i32_fsqu_trypush(&q, &v));/* 已满拒绝 */

    /* 满了之后要腾出多少才放得进,随后端的归还粒度走:queue+spin 与 mpq 是一个槽,
       bbq 是一整块。粒度不写死,量出来再拿它校验"腾出 N 个就恰好能再放进 N 个" */
    do {
        CuAssertTrue(tc, ERR_OK == i32_fsqu_pop(&q, &out));
        CuAssertTrue(tc, out == (int32_t)grain + 1);/* 出队仍是 FIFO */
        grain++;
        CuAssertTrue(tc, grain <= 8);
        v = 9;
    } while (ERR_FAILED == i32_fsqu_trypush(&q, &v));/* 成功的那次已占掉一个位置 */
    for (k = 1; k < grain; k++) {
        v = 9 + (int32_t)k;
        CuAssertTrue(tc, ERR_OK == i32_fsqu_trypush(&q, &v));
    }
    v = 99;
    CuAssertTrue(tc, ERR_FAILED == i32_fsqu_trypush(&q, &v));/* 又装满了,再放就该拒 */

    /* 剩余出队顺序为 grain+1..8,接 9..9+grain-1 */
    for (v = (int32_t)grain + 1; v <= 8; v++) {
        CuAssertTrue(tc, ERR_OK == i32_fsqu_pop(&q, &out) && out == v);
    }
    for (k = 0; k < grain; k++) {
        CuAssertTrue(tc, ERR_OK == i32_fsqu_pop(&q, &out) && out == 9 + (int32_t)k);
    }
    CuAssertTrue(tc, ERR_FAILED == i32_fsqu_pop(&q, &out));
    i32_fsqu_free(&q);
}

/* push_batch + pop_batch：批量入队 6 个、批量出队，返回实际数与顺序值正确；max 大于剩余只取剩余 */
static void test_fsqu_batch(CuTest *tc) {
    i32_fsqu q;
    int32_t in[6] = { 10, 20, 30, 40, 50, 60 };
    int32_t out[8];
    uint32_t n;
    i32_fsqu_init(&q, 8);

    i32_fsqu_push_batch(&q, in, 6);
    CuAssertTrue(tc, 6 == i32_fsqu_size(&q));

    /* 一次最多取 4 个 */
    n = i32_fsqu_pop_batch(&q, out, 4);
    CuAssertTrue(tc, 4 == n);
    CuAssertTrue(tc, 10 == out[0] && 20 == out[1] && 30 == out[2] && 40 == out[3]);
    CuAssertTrue(tc, 2 == i32_fsqu_size(&q));

    /* max=8 但只剩 2 个，返回 2 */
    n = i32_fsqu_pop_batch(&q, out, 8);
    CuAssertTrue(tc, 2 == n);
    CuAssertTrue(tc, 50 == out[0] && 60 == out[1]);
    CuAssertTrue(tc, 0 == i32_fsqu_size(&q));

    /* 空队列批量出队返回 0 */
    n = i32_fsqu_pop_batch(&q, out, 8);
    CuAssertTrue(tc, 0 == n);
    i32_fsqu_free(&q);
}

/* pop_sc + pop_sc_batch：单消费者出队，顺序值正确；空队列返回 ERR_FAILED / 0 */
static void test_fsqu_pop_sc(CuTest *tc) {
    i32_fsqu q;
    int32_t v, out;
    int32_t outs[8];
    uint32_t n;
    i32_fsqu_init(&q, 8);

    CuAssertTrue(tc, ERR_FAILED == i32_fsqu_pop_sc(&q, &out));/* 空队列单消费者出队 */

    for (v = 1; v <= 6; v++) {
        i32_fsqu_push(&q, &v);
    }
    /* pop_sc 逐个出队顺序一致 */
    for (v = 1; v <= 3; v++) {
        CuAssertTrue(tc, ERR_OK == i32_fsqu_pop_sc(&q, &out) && out == v);
    }
    CuAssertTrue(tc, 3 == i32_fsqu_size(&q));

    /* pop_sc_batch 取走剩余 3 个 */
    n = i32_fsqu_pop_sc_batch(&q, outs, 8);
    CuAssertTrue(tc, 3 == n);
    CuAssertTrue(tc, 4 == outs[0] && 5 == outs[1] && 6 == outs[2]);
    CuAssertTrue(tc, 0 == i32_fsqu_size(&q));

    /* 空队列单消费者批量出队返回 0 */
    n = i32_fsqu_pop_sc_batch(&q, outs, 8);
    CuAssertTrue(tc, 0 == n);
    i32_fsqu_free(&q);
}

/* 默认容量：capacity=0 走默认值，仍可正常 push/pop */
static void test_fsqu_default_cap(CuTest *tc) {
    i32_fsqu q;
    int32_t v, out;
    i32_fsqu_init(&q, 0);/* 0 → 默认容量 */

    CuAssertTrue(tc, i32_fsqu_capacity(&q) > 0);
    CuAssertTrue(tc, 0 == i32_fsqu_size(&q));
    for (v = 1; v <= 16; v++) {
        CuAssertTrue(tc, ERR_OK == i32_fsqu_trypush(&q, &v));
    }
    CuAssertTrue(tc, 16 == i32_fsqu_size(&q));
    for (v = 1; v <= 16; v++) {
        CuAssertTrue(tc, ERR_OK == i32_fsqu_pop(&q, &out) && out == v);
    }
    CuAssertTrue(tc, 0 == i32_fsqu_size(&q));
    i32_fsqu_free(&q);
}

/* 溢出降级：超过快路径容量后跨界 FIFO 严格保序，size 须含溢出层。
   重点验证粘滞降级——溢出层非空期间新元素不得回填快路径，
   否则快路径被消费腾空后新元素会插到更早的溢出元素之前（FSQU_FAST_MODEL=0 分支为无界 queue，同样保序） */
static void test_fsqu_overflow_fifo(CuTest *tc) {
    i32_fsqu q;
    int32_t v, out;
    i32_fsqu_init(&q, 8);

    for (v = 1; v <= 10; v++) {/* 1..8 进快路径，9..10 落溢出层 */
        i32_fsqu_push(&q, &v);
    }
    CuAssertTrue(tc, 10 == i32_fsqu_size(&q));

    for (v = 1; v <= 3; v++) {/* 消费 3 个，快路径腾出空位 */
        CuAssertTrue(tc, ERR_OK == i32_fsqu_pop(&q, &out) && out == v);
    }
    v = 11;/* 溢出层仍非空：11 必须继续落溢出层，不得插队到 9 之前 */
    i32_fsqu_push(&q, &v);
    CuAssertTrue(tc, 8 == i32_fsqu_size(&q));

    for (v = 4; v <= 11; v++) {
        CuAssertTrue(tc, ERR_OK == i32_fsqu_pop(&q, &out) && out == v);
    }
    CuAssertTrue(tc, 0 == i32_fsqu_size(&q));
    CuAssertTrue(tc, ERR_FAILED == i32_fsqu_pop(&q, &out));

    /* 溢出层已排空，粘滞解除，后续入队恢复正常 */
    for (v = 1; v <= 8; v++) {
        i32_fsqu_push(&q, &v);
    }
    for (v = 1; v <= 8; v++) {
        CuAssertTrue(tc, ERR_OK == i32_fsqu_pop(&q, &out) && out == v);
    }
    i32_fsqu_free(&q);
}

/* 批量出队跨界补齐：一次 pop_batch/pop_sc_batch 须跨快路径与溢出层边界取满 max，
   否则调用方按返回 0 判空会漏掉溢出层里的元素 */
static void test_fsqu_overflow_pop_batch(CuTest *tc) {
    i32_fsqu q;
    int32_t v, out[12];
    uint32_t n, i;
    i32_fsqu_init(&q, 8);

    for (v = 1; v <= 12; v++) {
        i32_fsqu_push(&q, &v);
    }
    CuAssertTrue(tc, 12 == i32_fsqu_size(&q));
    n = i32_fsqu_pop_batch(&q, out, 12);/* 前 8 来自快路径，后 4 从溢出层续取 */
    CuAssertTrue(tc, 12 == n);
    for (i = 0; i < 12; i++) {
        CuAssertTrue(tc, out[i] == (int32_t)(i + 1));
    }
    CuAssertTrue(tc, 0 == i32_fsqu_size(&q));

    for (v = 1; v <= 12; v++) {
        i32_fsqu_push(&q, &v);
    }
    n = i32_fsqu_pop_sc_batch(&q, out, 12);
    CuAssertTrue(tc, 12 == n);
    for (i = 0; i < 12; i++) {
        CuAssertTrue(tc, out[i] == (int32_t)(i + 1));
    }
    CuAssertTrue(tc, 0 == i32_fsqu_size(&q));
    i32_fsqu_free(&q);
}

/* 批量入队跨界：一次 push_batch 超过快路径容量，余量整批落溢出层且保序 */
static void test_fsqu_overflow_push_batch(CuTest *tc) {
    i32_fsqu q;
    int32_t in[12], out, v;
    i32_fsqu_init(&q, 8);

    for (v = 0; v < 12; v++) {
        in[v] = v + 1;
    }
    i32_fsqu_push_batch(&q, in, 12);
    CuAssertTrue(tc, 12 == i32_fsqu_size(&q));
    for (v = 1; v <= 12; v++) {
        CuAssertTrue(tc, ERR_OK == i32_fsqu_pop(&q, &out) && out == v);
    }
    CuAssertTrue(tc, 0 == i32_fsqu_size(&q));
    i32_fsqu_free(&q);
}

/* 粘滞规则对 trypush 同样生效：溢出层非空期间即便快路径已被排空,trypush 也须拒绝,
   否则新元素会插到更早的溢出元素之前。FSQU_FAST_MODEL=0 分支为无界 queue、push 从不
   溢出,故其中那段 #if 仅在无锁后端下有实质意义,其余断言各后端通用 */
static void test_fsqu_trypush_sticky(CuTest *tc) {
    i32_fsqu q;
    int32_t v, out;
    i32_fsqu_init(&q, 8);

    for (v = 1; v <= 10; v++) {/* 1..8 进快路径，9..10 落溢出层 */
        i32_fsqu_push(&q, &v);
    }
    for (v = 1; v <= 8; v++) {/* 排空快路径，令 mpq 环空出来而溢出层仍存 9、10 */
        CuAssertTrue(tc, ERR_OK == i32_fsqu_pop(&q, &out) && out == v);
    }
    CuAssertTrue(tc, 2 == i32_fsqu_size(&q));
#if FSQU_FAST_MODEL
    v = 999;
    CuAssertTrue(tc, ERR_FAILED == i32_fsqu_trypush(&q, &v));/* 溢出层非空 → 拒绝 */
    CuAssertTrue(tc, 2 == i32_fsqu_size(&q));
#endif
    CuAssertTrue(tc, ERR_OK == i32_fsqu_pop(&q, &out) && 9 == out);
    CuAssertTrue(tc, ERR_OK == i32_fsqu_pop(&q, &out) && 10 == out);
    CuAssertTrue(tc, ERR_FAILED == i32_fsqu_pop(&q, &out));
    i32_fsqu_free(&q);
}

// 队满时 trypush 必须立即返回 ERR_FAILED 且不退避：fsqu 靠这个失败降级到无界溢出层，
// 满路径上多等一下都会把降级拖住
static void test_mpq_full_fails_fast(CuTest *tc) {
    u32_mpq q;
    uint64_t t0;
    uint32_t v;
    uint32_t bad = 0;
    int32_t i, cap;

    u32_mpq_init(&q, 64);
    cap = (int32_t)u32_mpq_capacity(&q);
    for (i = 0; i < cap; i++) {
        v = (uint32_t)i;
        CuAssertIntEquals(tc, ERR_OK, u32_mpq_trypush(&q, &v));
    }
    v = 0;
    t0 = nowms();
    for (i = 0; i < _MPQ_FULL_HITS; i++) {
        if (ERR_FAILED != u32_mpq_trypush(&q, &v)) {
            bad++;
        }
    }
    CuAssertIntEquals(tc, 0, (int32_t)bad);
    CuAssertTrue(tc, nowms() - t0 < _MPQ_FULL_MAXMS);
    u32_mpq_free(&q);
}

// 退避步长倍增到 MPQ_BACKOFF_CAP 为止，任何一步都不越过它，最后停在它上面。
// CAP 不是 2 的幂的平台（x86 上是 7）只靠倍增会冲过头；关了退避时步长恒为 0
static void test_mpq_backoff_cap(CuTest *tc) {
    uint32_t st = 0;
    int32_t k;
    for (k = 0; k < 40; k++) {
        _mpq_backoff(&st);
        CuAssertTrue(tc, st <= (uint32_t)MPQ_BACKOFF_CAP);
    }
    CuAssertTrue(tc, (0 == MPQ_BACKOFF_CYCLES ? 0u : (uint32_t)MPQ_BACKOFF_CAP) == st);
}

/* uptr_mpq_pop / uptr_mpq_pop_sc 的第三种返回值：1 表示"看似空但有槽位已被抢占尚未发布"。
   单线程造不出抢占窗口，这里验的是它的确定端点——单线程下永远不该出现 1，
   空队列必须是 ERR_FAILED，fsqu 的溢出层守卫就建在这个区分上 */
static void test_mpq_pop_empty_vs_inflight(CuTest *tc) {
    uptr_mpq q;
    uintptr_t v, out;
    uptr_mpq_init(&q, 8);

    CuAssertIntEquals(tc, ERR_FAILED, uptr_mpq_pop(&q, &out));/* 真空：enq == deq */
    CuAssertIntEquals(tc, ERR_FAILED, uptr_mpq_pop_sc(&q, &out));
    v = 1;
    CuAssertTrue(tc, ERR_OK == uptr_mpq_trypush(&q, &v));
    CuAssertTrue(tc, ERR_OK == uptr_mpq_pop(&q, &out) && 1 == out);
    CuAssertIntEquals(tc, ERR_FAILED, uptr_mpq_pop(&q, &out));/* 取完复归真空 */
    v = 2;
    CuAssertTrue(tc, ERR_OK == uptr_mpq_trypush(&q, &v));
    CuAssertTrue(tc, ERR_OK == uptr_mpq_pop_sc(&q, &out) && 2 == out);
    CuAssertIntEquals(tc, ERR_FAILED, uptr_mpq_pop_sc(&q, &out));
    uptr_mpq_free(&q);
}

/* 并发：4 生产者 × 1 消费者，验元素守恒，顺带观察 u32_mpq_pop 的在途态（返回 1）。
   容量压到 64 是刻意的：多个生产者反复抢同一批槽位，抢占窗口才够密。
   消费者必须与生产者并发跑、靠计数收尾——等 join 完再排空的话，那时已无在途 push，
   一次都观察不到。在途计数只打印不断言：单核机器上它合法地就是 0 */
static atomic_t _mpqmp_stop;

static void _mpqmp_producer(void *ud) {
    mpqmp_arg *a = (mpqmp_arg *)ud;
    uint32_t v;
    for (uint32_t i = 0; i < _MPQMP_PER; i++) {
        v = a->base + i;
        while (ERR_OK != u32_mpq_trypush(a->q, &v)) {
            /* 消费者超时会置 stop；不给这条出路的话满队自旋会让 join 永远回不来 */
            if (0 != ATOMIC_GET(&_mpqmp_stop)) {
                return;
            }
            CPU_PAUSE();
        }
    }
}

static void test_mpq_multiprod_conserve(CuTest *tc) {
    u32_mpq q;
    mpqmp_arg args[_MPQMP_PROD];
    pthread_t ths[_MPQMP_PROD];
    uint32_t v, got = 0, dup = 0, inflight = 0, empty = 0, bad = 0, fails = 0;
    int32_t rtn, i;
    char *seen;

    u32_mpq_init(&q, _MPQMP_CAP);
    CALLOC(seen, 1, _MPQMP_TOTAL);
    ATOMIC_SET(&_mpqmp_stop, 0);
    for (i = 0; i < _MPQMP_PROD; i++) {
        args[i].base = (uint32_t)i * _MPQMP_PER;
        args[i].q = &q;
        ths[i] = thread_creat(_mpqmp_producer, &args[i]);
    }
    uint64_t deadline = nowms() + _MPQ_MAXMS;
    while (got < _MPQMP_TOTAL) {
        rtn = u32_mpq_pop(&q, &v);
        if (ERR_OK == rtn) {
            /* 越界或重复都记进 dup：got 数满且 dup 为 0 即等价于"每个值恰好出来一次" */
            if (v >= _MPQMP_TOTAL || 0 != seen[v]) {
                dup++;
            } else {
                seen[v] = 1;
            }
            got++;
            continue;
        }
        if (1 == rtn) {
            inflight++;
        } else if (ERR_FAILED == rtn) {
            empty++;
        } else {
            bad++;
        }
        /* 每约 100 万次空转才看一次表：取不到时是紧循环，逐次 nowms 会拖慢整个用例 */
        if (0 == (++fails & 0xFFFFF)
            && nowms() > deadline) {
            ATOMIC_SET(&_mpqmp_stop, 1);
            break;
        }
    }
    for (i = 0; i < _MPQMP_PROD; i++) {
        thread_join(ths[i]);
    }
    /* 先收拾再断言：CuTest 断言失败是 longjmp 出去的，放在断言后面就漏了 */
    FREE(seen);
    u32_mpq_free(&q);
    LOG_INFO("[mpq] multiprod: got=%u inflight=%u empty=%u", got, inflight, empty);
    CuAssertIntEquals(tc, 0, (int32_t)bad);/* 返回值只落在三态内 */
    CuAssertIntEquals(tc, 0, (int32_t)dup);/* 不重复、不越界 */
    CuAssertIntEquals(tc, _MPQMP_TOTAL, (int32_t)got);/* 不丢失 */
}

/* =======================================================================
 * bbq —— 分块无锁多生产者有界队列（接口与 mpq 一致，行为差异见 bbq.h 文件头）
 * ======================================================================= */

/* 单线程：基本入队出队、FIFO 顺序（uptr_bbq_pop 路径） */
static void test_bbq_basic(CuTest *tc) {
    uptr_bbq q;
    uintptr_t v, out;
    uint32_t i;

    uptr_bbq_init(&q, 0);/* 0 → 默认容量 1024 */
    CuAssertTrue(tc, 1024 == uptr_bbq_capacity(&q));
    CuAssertTrue(tc, sizeof(uintptr_t) == uptr_bbq_elsize(&q));
    CuAssertTrue(tc, 0 == uptr_bbq_size(&q));
    CuAssertTrue(tc, 0 != uptr_bbq_empty(&q));
    CuAssertIntEquals(tc, ERR_FAILED, uptr_bbq_pop(&q, &out));

    for (i = 0; i < 10; i++) {
        v = i;
        CuAssertIntEquals(tc, ERR_OK, uptr_bbq_trypush(&q, &v));
    }
    CuAssertTrue(tc, 10 == uptr_bbq_size(&q));
    CuAssertTrue(tc, 0 == uptr_bbq_empty(&q));
    for (i = 0; i < 10; i++) {
        CuAssertTrue(tc, ERR_OK == uptr_bbq_pop(&q, &out) && out == i);
    }
    CuAssertTrue(tc, 0 == uptr_bbq_size(&q));
    CuAssertIntEquals(tc, ERR_FAILED, uptr_bbq_pop(&q, &out));
    uptr_bbq_free(&q);
}

/* 单线程：基本入队出队、FIFO 顺序（uptr_bbq_pop_sc 路径，独立验证单消费者算法） */
static void test_bbq_basic_sc(CuTest *tc) {
    uptr_bbq q;
    uintptr_t v, out;
    uint32_t i;

    uptr_bbq_init(&q, 0);
    CuAssertIntEquals(tc, ERR_FAILED, uptr_bbq_pop_sc(&q, &out));
    for (i = 0; i < 10; i++) {
        v = i;
        CuAssertIntEquals(tc, ERR_OK, uptr_bbq_trypush(&q, &v));
    }
    for (i = 0; i < 10; i++) {
        CuAssertTrue(tc, ERR_OK == uptr_bbq_pop_sc(&q, &out) && out == i);
    }
    CuAssertTrue(tc, 0 == uptr_bbq_size(&q));
    CuAssertIntEquals(tc, ERR_FAILED, uptr_bbq_pop_sc(&q, &out));
    uptr_bbq_free(&q);
}

/* 容量取整 + 队满即失败 + 空间按块归还（bbq 独有：消费掉整整一块才放行） */
static void test_bbq_boundary(CuTest *tc) {
    u32_bbq q;
    uint32_t v, out, i, cap, blksz;

    /* 不足最小容量与非 2 的幂都往上取整 */
    u32_bbq_init(&q, 5);
    CuAssertTrue(tc, 8 == u32_bbq_capacity(&q));
    u32_bbq_free(&q);
    u32_bbq_init(&q, 100);
    CuAssertTrue(tc, 128 == u32_bbq_capacity(&q));
    u32_bbq_free(&q);

    u32_bbq_init(&q, 64);
    cap = u32_bbq_capacity(&q);
    blksz = q.blksz;
    CuAssertTrue(tc, 64 == cap);
    CuAssertTrue(tc, blksz >= 2 && cap == q.nblk * blksz);
    for (i = 0; i < cap; i++) {
        v = i;
        CuAssertIntEquals(tc, ERR_OK, u32_bbq_trypush(&q, &v));
    }
    CuAssertTrue(tc, cap == u32_bbq_size(&q));
    v = 999;
    CuAssertIntEquals(tc, ERR_FAILED, u32_bbq_trypush(&q, &v));
    /* 消费掉一块差一条：仍然满 */
    for (i = 0; i + 1 < blksz; i++) {
        CuAssertTrue(tc, ERR_OK == u32_bbq_pop(&q, &out) && out == i);
    }
    CuAssertIntEquals(tc, ERR_FAILED, u32_bbq_trypush(&q, &v));
    /* 补上最后一条，整块归还，立刻能入队 */
    CuAssertTrue(tc, ERR_OK == u32_bbq_pop(&q, &out) && out == blksz - 1);
    CuAssertIntEquals(tc, ERR_OK, u32_bbq_trypush(&q, &v));
    u32_bbq_free(&q);
}

/* 三态：真空 / 在途。在途的两个入口都要钉住——第二个是与 mpq 的行为差异：
   本块已有写好的条目，只要另有一个生产者在途，整块都取不到。
   pop 与 pop_sc 不能在同一队列上混用（单消费者路径不维护 reserved），故两种出队各用一个队列走一遍 */
static int32_t _bbq_pop_mode(uptr_bbq *q, uintptr_t *out, int32_t sc) {
    return sc ? uptr_bbq_pop_sc(q, out) : uptr_bbq_pop(q, out);
}
static void _bbq_empty_vs_inflight_case(CuTest *tc, int32_t sc) {
    uptr_bbq q;
    uintptr_t v, out;
    uint32_t idx;

    uptr_bbq_init(&q, 64);
    CuAssertTrue(tc, q.blksz > 2);
    CuAssertIntEquals(tc, ERR_FAILED, _bbq_pop_mode(&q, &out, sc));

    /* 入口 1：空块上有人抢了槽还没写完。直接改游标模拟在途生产者，不起线程 */
    idx = (uint32_t)(ATOMIC64_GET(&q.phead) & q.idxmask);
    ATOMIC64_ADD(&q.blocks[idx].allocated, 1);
    CuAssertIntEquals(tc, 1, _bbq_pop_mode(&q, &out, sc));
    CuAssertIntEquals(tc, 1, _bbq_pop_mode(&q, &out, sc));/* 在途时反复查不消费 */
    CuAssertTrue(tc, 0 == uptr_bbq_empty(&q));
    CuAssertTrue(tc, uptr_bbq_size(&q) >= 1);
    /* 替它把数据补上并发布，队列回到正常 */
    q.cells[(size_t)idx * q.blksz] = 7;
    ATOMIC64_ADD(&q.blocks[idx].committed, 1);
    CuAssertTrue(tc, ERR_OK == _bbq_pop_mode(&q, &out, sc) && 7 == out);

    /* 入口 2：本块有已写好的条目，但另有在途 → 整块封锁 */
    v = 11;
    CuAssertIntEquals(tc, ERR_OK, uptr_bbq_trypush(&q, &v));
    idx = (uint32_t)(ATOMIC64_GET(&q.phead) & q.idxmask);
    ATOMIC64_ADD(&q.blocks[idx].allocated, 1);
    CuAssertIntEquals(tc, 1, _bbq_pop_mode(&q, &out, sc));
    CuAssertIntEquals(tc, 1, _bbq_pop_mode(&q, &out, sc));
    /* 在途那条补齐后，两条按 FIFO 出来 */
    q.cells[(size_t)idx * q.blksz + 2] = 13;
    ATOMIC64_ADD(&q.blocks[idx].committed, 1);
    CuAssertTrue(tc, ERR_OK == _bbq_pop_mode(&q, &out, sc) && 11 == out);
    CuAssertTrue(tc, ERR_OK == _bbq_pop_mode(&q, &out, sc) && 13 == out);
    CuAssertIntEquals(tc, ERR_FAILED, _bbq_pop_mode(&q, &out, sc));
    uptr_bbq_free(&q);
}
static void test_bbq_pop_empty_vs_inflight(CuTest *tc) {
    _bbq_empty_vs_inflight_case(tc, 0);
    _bbq_empty_vs_inflight_case(tc, 1);
}

/* 换代不卡死：最小容量下反复走完整个块环，块序号要一路推上去且 FIFO 不乱 */
static void test_bbq_epoch_wrap(CuTest *tc) {
    u32_bbq q;
    uint32_t v, out, i;
    uint64_t head0;

    u32_bbq_init(&q, 8);
    head0 = (uint64_t)ATOMIC64_GET(&q.phead);
    for (i = 0; i < _BBQ_WRAP_ROUNDS; i++) {
        v = i;
        CuAssertIntEquals(tc, ERR_OK, u32_bbq_trypush(&q, &v));
        CuAssertTrue(tc, ERR_OK == u32_bbq_pop_sc(&q, &out) && out == i);
    }
    /* 每 blksz 条推一个块，块序号必须真的涨上去（不涨就是退化成只用一个块） */
    CuAssertTrue(tc, (uint64_t)ATOMIC64_GET(&q.phead)
                     >= head0 + _BBQ_WRAP_ROUNDS / q.blksz - 1);
    CuAssertTrue(tc, 0 == u32_bbq_size(&q));
    u32_bbq_free(&q);
}

/* 并发：4 生产者 × 1 消费者，验元素守恒，顺带统计三态。结构同 mpq 的同名用例 */
static atomic_t _bbqmp_stop;

static void _bbqmp_producer(void *ud) {
    bbqmp_arg *a = (bbqmp_arg *)ud;
    uint32_t v, i;
    for (i = 0; i < _BBQMP_PER; i++) {
        v = a->base + i;
        while (ERR_OK != u32_bbq_trypush(a->q, &v)) {
            /* 消费者超时会置 stop；不给这条出路的话满队自旋会让 join 永远回不来 */
            if (0 != ATOMIC_GET(&_bbqmp_stop)) {
                return;
            }
            CPU_PAUSE();
        }
    }
}

static void test_bbq_multiprod_conserve(CuTest *tc) {
    u32_bbq q;
    bbqmp_arg args[_BBQMP_PROD];
    pthread_t ths[_BBQMP_PROD];
    uint32_t v, got = 0, dup = 0, inflight = 0, empty = 0, bad = 0, fails = 0;
    uint64_t deadline;
    int32_t rtn, i;
    char *seen;

    u32_bbq_init(&q, _BBQMP_CAP);
    CALLOC(seen, 1, _BBQMP_TOTAL);
    ATOMIC_SET(&_bbqmp_stop, 0);
    for (i = 0; i < _BBQMP_PROD; i++) {
        args[i].base = (uint32_t)i * _BBQMP_PER;
        args[i].q = &q;
        ths[i] = thread_creat(_bbqmp_producer, &args[i]);
    }
    deadline = nowms() + _MPQ_MAXMS;
    while (got < _BBQMP_TOTAL) {
        rtn = u32_bbq_pop(&q, &v);
        if (ERR_OK == rtn) {
            /* 越界或重复都记进 dup：got 数满且 dup 为 0 即等价于"每个值恰好出来一次" */
            if (v >= _BBQMP_TOTAL || 0 != seen[v]) {
                dup++;
            } else {
                seen[v] = 1;
            }
            got++;
            continue;
        }
        if (1 == rtn) {
            inflight++;
        } else if (ERR_FAILED == rtn) {
            empty++;
        } else {
            bad++;
        }
        /* 每约 100 万次空转才看一次表：取不到时是紧循环，逐次 nowms 会拖慢整个用例 */
        if (0 == (++fails & 0xFFFFF)
            && nowms() > deadline) {
            ATOMIC_SET(&_bbqmp_stop, 1);
            break;
        }
    }
    for (i = 0; i < _BBQMP_PROD; i++) {
        thread_join(ths[i]);
    }
    /* 先收拾再断言：CuTest 断言失败是 longjmp 出去的，放在断言后面就漏了 */
    FREE(seen);
    u32_bbq_free(&q);
    LOG_INFO("[bbq] multiprod: got=%u inflight=%u empty=%u", got, inflight, empty);
    CuAssertIntEquals(tc, 0, (int32_t)bad);/* 返回值只落在三态内 */
    CuAssertIntEquals(tc, 0, (int32_t)dup);/* 不重复、不越界 */
    CuAssertIntEquals(tc, _BBQMP_TOTAL, (int32_t)got);/* 不丢失 */
}

/* 并发：4 生产者 × 3 消费者，消费者全走多消费者 pop（CAS 预订槽位），验元素守恒。
   seen 按值计数，两个消费者拿到同一个值就是重复 */
static atomic_t _bbqmc_got;
static atomic_t _bbqmc_dup;
static atomic_t *_bbqmc_seen;
static void _bbqmc_consumer(void *ud) {
    u32_bbq *q = (u32_bbq *)ud;
    uint32_t v;
    uint32_t fails = 0;
    uint64_t deadline = nowms() + _MPQ_MAXMS;
    while (ATOMIC_GET(&_bbqmc_got) < (atomic_t)_BBQMP_TOTAL) {
        if (ERR_OK == u32_bbq_pop(q, &v)) {
            if (v >= _BBQMP_TOTAL || 0 != ATOMIC_ADD(&_bbqmc_seen[v], 1)) {
                ATOMIC_ADD(&_bbqmc_dup, 1);
            }
            ATOMIC_ADD(&_bbqmc_got, 1);
            continue;
        }
        /* 到点置 stop 让生产者也能退出 */
        if (0 == (++fails & 0xFFFFF)
            && nowms() > deadline) {
            ATOMIC_SET(&_bbqmp_stop, 1);
            break;
        }
        CPU_PAUSE();
    }
}
static void test_bbq_multicons_conserve(CuTest *tc) {
    u32_bbq q;
    bbqmp_arg args[_BBQMP_PROD];
    pthread_t prods[_BBQMP_PROD];
    pthread_t cons[_BBQMC_CONS];
    int32_t i;

    u32_bbq_init(&q, _BBQMP_CAP);
    CALLOC(_bbqmc_seen, _BBQMP_TOTAL, sizeof(atomic_t));
    ATOMIC_SET(&_bbqmp_stop, 0);
    ATOMIC_SET(&_bbqmc_got, 0);
    ATOMIC_SET(&_bbqmc_dup, 0);
    for (i = 0; i < _BBQMC_CONS; i++) {
        cons[i] = thread_creat(_bbqmc_consumer, &q);
    }
    for (i = 0; i < _BBQMP_PROD; i++) {
        args[i].base = (uint32_t)i * _BBQMP_PER;
        args[i].q = &q;
        prods[i] = thread_creat(_bbqmp_producer, &args[i]);
    }
    for (i = 0; i < _BBQMP_PROD; i++) {
        thread_join(prods[i]);
    }
    for (i = 0; i < _BBQMC_CONS; i++) {
        thread_join(cons[i]);
    }
    FREE(_bbqmc_seen);
    u32_bbq_free(&q);
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_bbqmc_dup));/* 不重复、不越界 */
    CuAssertIntEquals(tc, _BBQMP_TOTAL, (int32_t)ATOMIC_GET(&_bbqmc_got));/* 不丢失 */
}

/* u32_bbq_size 只高估不低估：队列恒非空时不得读出 0。同 mpq 的同名用例，
   churn 一次进出整整一块——bbq 空间按块归还，只退一条是换不回入队名额的 */
static atomic_t _bbq_sz_zero;
static atomic_t _bbq_sz_stop;
static atomic_t _bbq_sz_fail;// churn 入队重试超时置 1
static void _bbq_size_churn(void *arg) {
    uptr_bbq *q = (uptr_bbq *)arg;
    uintptr_t p = 0;
    uint32_t i, k, spins = 0;
    uint64_t deadline = nowms() + _MPQ_MAXMS;
    for (i = 0; i < _BBQ_SZ_ROUNDS; i++) {
        for (k = 0; k < q->blksz; k++) {
            if (ERR_OK != uptr_bbq_pop_sc(q, &p)) {
                break;
            }
        }
        for (; k > 0; k--) {
            while (ERR_OK != uptr_bbq_trypush(q, &p)) {
                // 块迟迟回收不了就判失败退出，不然 churn 与 watch 一起挂死、join 回不来
                if (0 == (++spins & 0xFFFFF)
                    && nowms() > deadline) {
                    ATOMIC_SET(&_bbq_sz_fail, 1);
                    ATOMIC_SET(&_bbq_sz_stop, 1);
                    return;
                }
                CPU_PAUSE();
            }
        }
    }
    ATOMIC_SET(&_bbq_sz_stop, 1);
}
static void _bbq_size_watch(void *arg) {
    uptr_bbq *q = (uptr_bbq *)arg;
    while (0 == ATOMIC_GET(&_bbq_sz_stop)) {
        if (0 == uptr_bbq_size(q)) {
            ATOMIC_ADD(&_bbq_sz_zero, 1);
        }
        // 纯热转会把 churn 线程要读写的那几条缓存行占死, 单核上更是直接饿着它
        CPU_PAUSE();
    }
}
static void test_bbq_size_never_underreports(CuTest *tc) {
    uptr_bbq q;
    pthread_t churn, watch;
    uintptr_t v;
    uint32_t i, cap;

    uptr_bbq_init(&q, _BBQ_SZ_CAP);
    cap = uptr_bbq_capacity(&q);
    for (i = 0; i < cap; i++) {
        v = i + 1;
        CuAssertIntEquals(tc, ERR_OK, uptr_bbq_trypush(&q, &v));
    }
    ATOMIC_SET(&_bbq_sz_zero, 0);
    ATOMIC_SET(&_bbq_sz_stop, 0);
    ATOMIC_SET(&_bbq_sz_fail, 0);
    churn = thread_creat(_bbq_size_churn, &q);
    watch = thread_creat(_bbq_size_watch, &q);
    thread_join(churn);
    thread_join(watch);
    // 往返是配平的，跑完应回到满
    i = uptr_bbq_size(&q);
    uptr_bbq_free(&q);
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_bbq_sz_fail));
    CuAssertIntEquals(tc, (int32_t)cap, (int32_t)i);
    CuAssertIntEquals(tc, 0, (int32_t)ATOMIC_GET(&_bbq_sz_zero));
}

/* 搭"推块做到一半"的现场：容量 8 恒为 4 块 x 2 槽；0 号块写满又消费完（phead == chead），
   生产者 A 给下一块换了代（committed，alloc 非 0 时连 allocated）但还没推 phead。返回下一块下标 */
static uint32_t _bbq_half_advance(CuTest *tc, u32_bbq *q, int32_t alloc) {
    uint32_t v, out, i;
    uint64_t phd;
    u32_bbq_block *nb;
    u32_bbq_init(q, 8);
    CuAssertTrue(tc, 4 == q->nblk && 2 == q->blksz);
    for (i = 0; i < 2; i++) {
        v = i;
        CuAssertIntEquals(tc, ERR_OK, u32_bbq_trypush(q, &v));
    }
    for (i = 0; i < 2; i++) {
        CuAssertTrue(tc, ERR_OK == u32_bbq_pop_sc(q, &out) && out == i);
    }
    phd = (uint64_t)ATOMIC64_GET(&q->phead);
    CuAssertTrue(tc, phd == (uint64_t)ATOMIC64_GET(&q->chead));
    nb = &q->blocks[(phd + 1) & q->idxmask];
    _bbq_maxset(&nb->committed, BBQ_CUR(phd + 1, 0));
    if (alloc) {
        _bbq_maxset(&nb->allocated, BBQ_CUR(phd + 1, 0));
    }
    return (uint32_t)((phd + 1) & q->idxmask);
}
/* 落后整圈的生产者 S 在新块上抢槽、写数据、发布 */
static void _bbq_stale_write(u32_bbq *q, uint32_t idx, uint32_t v) {
    uint64_t old = (uint64_t)ATOMIC64_ADD(&q->blocks[idx].allocated, 1);
    q->cells[(size_t)idx * q->blksz + BBQ_OFF(old)] = v;
    ATOMIC64_ADD(&q->blocks[idx].committed, 1);
}
/* size / empty 不得低估：phead 还停在满块上时，数据已经写进下一块。
   只按 phead 算会把非空报成 0，fsqu 靠它判断睡不睡，报 0 就漏唤醒 */
static void test_bbq_size_half_advance(CuTest *tc) {
    u32_bbq q;
    uint32_t idx, out;

    /* 生产头落后：S 写进一条，phead 未动 */
    idx = _bbq_half_advance(tc, &q, 1);
    _bbq_stale_write(&q, idx, 777);
    CuAssertTrue(tc, u32_bbq_size(&q) >= 1);
    CuAssertTrue(tc, 0 == u32_bbq_empty(&q));
    CuAssertTrue(tc, ERR_OK == u32_bbq_pop_sc(&q, &out) && 777 == out);
    u32_bbq_free(&q);

    /* 消费头领先：S 写两条，消费者取走一条后 chead 跑到 phead 前面一块 */
    idx = _bbq_half_advance(tc, &q, 1);
    _bbq_stale_write(&q, idx, 777);
    _bbq_stale_write(&q, idx, 778);
    CuAssertTrue(tc, ERR_OK == u32_bbq_pop_sc(&q, &out) && 777 == out);
    CuAssertTrue(tc, (uint64_t)ATOMIC64_GET(&q.chead) == (uint64_t)ATOMIC64_GET(&q.phead) + 1);
    CuAssertTrue(tc, u32_bbq_size(&q) >= 1);
    CuAssertTrue(tc, 0 == u32_bbq_empty(&q));
    CuAssertTrue(tc, ERR_OK == u32_bbq_pop_sc(&q, &out) && 778 == out);
    u32_bbq_free(&q);

    /* 真空：A 只换了 committed、没人写，pop_sc 报在途，size 不该凭空报出元素 */
    (void)_bbq_half_advance(tc, &q, 0);
    CuAssertIntEquals(tc, 1, u32_bbq_pop_sc(&q, &out));
    CuAssertIntEquals(tc, 0, (int32_t)u32_bbq_size(&q));
    u32_bbq_free(&q);
}

/* 批量出队(单消费者):取数、顺序、rtn 三态、环绕。mpq 与 bbq 两份签名一致，用同一组断言 */
static void test_mpq_pop_sc_batch(CuTest *tc) {
    u32_mpq q;
    uint32_t v, out[16];
    uint32_t i, n;
    int32_t rtn;

    u32_mpq_init(&q, 8);
    /* 空队列：取 0 条，rtn 报真空 */
    rtn = ERR_OK;
    CuAssertIntEquals(tc, 0, (int32_t)u32_mpq_pop_sc_batch(&q, out, 16, &rtn));
    CuAssertIntEquals(tc, ERR_FAILED, rtn);
    /* max=0 是合法调用：不取、不报空 */
    rtn = ERR_FAILED;
    CuAssertIntEquals(tc, 0, (int32_t)u32_mpq_pop_sc_batch(&q, out, 0, &rtn));
    CuAssertIntEquals(tc, ERR_OK, rtn);

    for (i = 0; i < 6; i++) {
        v = i;
        CuAssertIntEquals(tc, ERR_OK, u32_mpq_trypush(&q, &v));
    }
    /* 取满 max：rtn 必须是 ERR_OK，否则 fsqu 会误判成"队列空了"去排溢出层 */
    rtn = ERR_FAILED;
    n = u32_mpq_pop_sc_batch(&q, out, 4, &rtn);
    CuAssertIntEquals(tc, 4, (int32_t)n);
    CuAssertIntEquals(tc, ERR_OK, rtn);
    for (i = 0; i < 4; i++) {
        CuAssertIntEquals(tc, (int32_t)i, (int32_t)out[i]);
    }
    /* 取不满：拿到剩下的，rtn 报真空 */
    rtn = ERR_OK;
    n = u32_mpq_pop_sc_batch(&q, out, 16, &rtn);
    CuAssertIntEquals(tc, 2, (int32_t)n);
    CuAssertIntEquals(tc, ERR_FAILED, rtn);
    CuAssertIntEquals(tc, 4, (int32_t)out[0]);
    CuAssertIntEquals(tc, 5, (int32_t)out[1]);
    CuAssertTrue(tc, 0 == u32_mpq_size(&q));

    /* 反复绕环：每轮的批量都跨过环绕点，顺序不能乱 */
    for (i = 0; i < _MPQ_BATCH_WRAP; i++) {
        v = i;
        CuAssertIntEquals(tc, ERR_OK, u32_mpq_trypush(&q, &v));
        v = i + 1000000;
        CuAssertIntEquals(tc, ERR_OK, u32_mpq_trypush(&q, &v));
        rtn = ERR_OK;
        n = u32_mpq_pop_sc_batch(&q, out, 16, &rtn);
        CuAssertIntEquals(tc, 2, (int32_t)n);
        CuAssertIntEquals(tc, (int32_t)i, (int32_t)out[0]);
        CuAssertIntEquals(tc, (int32_t)(i + 1000000), (int32_t)out[1]);
    }
    u32_mpq_free(&q);
}

static void test_bbq_pop_sc_batch(CuTest *tc) {
    u32_bbq q;
    uint32_t v, out[512];
    uint32_t i, n, cap;
    int32_t rtn;

    u32_bbq_init(&q, 64);
    cap = u32_bbq_capacity(&q);
    CuAssertTrue(tc, q.blksz > 2 && q.nblk > 1);
    rtn = ERR_OK;
    CuAssertIntEquals(tc, 0, (int32_t)u32_bbq_pop_sc_batch(&q, out, 16, &rtn));
    CuAssertIntEquals(tc, ERR_FAILED, rtn);
    rtn = ERR_FAILED;
    CuAssertIntEquals(tc, 0, (int32_t)u32_bbq_pop_sc_batch(&q, out, 0, &rtn));
    CuAssertIntEquals(tc, ERR_OK, rtn);

    /* 灌满：一次批量要能跨过所有块取回来 */
    for (i = 0; i < cap; i++) {
        v = i;
        CuAssertIntEquals(tc, ERR_OK, u32_bbq_trypush(&q, &v));
    }
    rtn = ERR_OK;
    n = u32_bbq_pop_sc_batch(&q, out, 512, &rtn);
    CuAssertIntEquals(tc, (int32_t)cap, (int32_t)n);
    CuAssertIntEquals(tc, ERR_FAILED, rtn);
    for (i = 0; i < cap; i++) {
        CuAssertIntEquals(tc, (int32_t)i, (int32_t)out[i]);
    }
    /* 取满 max 时 rtn 必须是 ERR_OK */
    for (i = 0; i < 10; i++) {
        v = i + 100;
        CuAssertIntEquals(tc, ERR_OK, u32_bbq_trypush(&q, &v));
    }
    rtn = ERR_FAILED;
    n = u32_bbq_pop_sc_batch(&q, out, 3, &rtn);
    CuAssertIntEquals(tc, 3, (int32_t)n);
    CuAssertIntEquals(tc, ERR_OK, rtn);
    CuAssertIntEquals(tc, 100, (int32_t)out[0]);

    /* 整块封锁在批量路径上同样先生效：本块另有一个在途生产者时，连已经写好的 7 条
       也一条都取不到。批量不能绕过这条，绕过就读到没写完的槽 */
    i = (uint32_t)(ATOMIC64_GET(&q.phead) & q.idxmask);
    ATOMIC64_ADD(&q.blocks[i].allocated, 1);
    rtn = ERR_OK;
    n = u32_bbq_pop_sc_batch(&q, out, 512, &rtn);
    CuAssertIntEquals(tc, 0, (int32_t)n);
    CuAssertIntEquals(tc, ERR_FAILED, rtn);/* 批量的 rtn 只分取满/没取满 */
    /* 要区分在途得自己补一次单条，那条才给三态 */
    CuAssertIntEquals(tc, 1, u32_bbq_pop_sc(&q, &v));
    /* 在途那条补齐后，7 条旧的加它自己一起出来，顺序不变 */
    q.cells[(size_t)i * q.blksz + 10] = 999;
    ATOMIC64_ADD(&q.blocks[i].committed, 1);
    rtn = ERR_OK;
    n = u32_bbq_pop_sc_batch(&q, out, 512, &rtn);
    CuAssertIntEquals(tc, 8, (int32_t)n);
    CuAssertIntEquals(tc, ERR_FAILED, rtn);
    for (i = 0; i < 7; i++) {
        CuAssertIntEquals(tc, (int32_t)(103 + i), (int32_t)out[i]);
    }
    CuAssertIntEquals(tc, 999, (int32_t)out[7]);
    u32_bbq_free(&q);
}

/* 并发：4 生产者 × 1 消费者走批量路径，验元素守恒。批量把"多条一起搬"塞进了
   单消费者路径，丢条/重复只会在这里现形 */
static void test_mpq_batch_conserve(CuTest *tc) {
    u32_mpq q;
    mpqmp_arg args[_MPQMP_PROD];
    pthread_t ths[_MPQMP_PROD];
    uint32_t batch[_MPQ_BATCH_MAX];
    uint32_t got = 0, dup = 0, fails = 0, i, n, k;
    uint64_t deadline;
    int32_t rtn, j;
    char *seen;

    u32_mpq_init(&q, _MPQMP_CAP);
    CALLOC(seen, 1, _MPQMP_TOTAL);
    ATOMIC_SET(&_mpqmp_stop, 0);
    for (j = 0; j < _MPQMP_PROD; j++) {
        args[j].base = (uint32_t)j * _MPQMP_PER;
        args[j].q = &q;
        ths[j] = thread_creat(_mpqmp_producer, &args[j]);
    }
    deadline = nowms() + _MPQ_MAXMS;
    while (got < _MPQMP_TOTAL) {
        k = _MPQMP_TOTAL - got;
        if (k > _MPQ_BATCH_MAX) {
            k = _MPQ_BATCH_MAX;
        }
        n = u32_mpq_pop_sc_batch(&q, batch, k, &rtn);
        for (i = 0; i < n; i++) {
            if (batch[i] >= _MPQMP_TOTAL || 0 != seen[batch[i]]) {
                dup++;
            } else {
                seen[batch[i]] = 1;
            }
        }
        got += n;
        if (0 != n) {
            continue;
        }
        if (0 == (++fails & 0xFFFFF)
            && nowms() > deadline) {
            ATOMIC_SET(&_mpqmp_stop, 1);
            break;
        }
    }
    for (j = 0; j < _MPQMP_PROD; j++) {
        thread_join(ths[j]);
    }
    FREE(seen);
    u32_mpq_free(&q);
    CuAssertIntEquals(tc, 0, (int32_t)dup);
    CuAssertIntEquals(tc, _MPQMP_TOTAL, (int32_t)got);
}

/* 溢出层守卫不得误伤正常路径：mpq 确实空了（enq == deq）时，溢出层必须照常排空。
   守卫写错方向的话这里会一个都取不出来 */
static void test_fsqu_ovf_drain_after_mpq_empty(CuTest *tc) {
    i32_fsqu q;
    int32_t v, out;
    uint32_t i, n;
    int32_t batch[4];
    i32_fsqu_init(&q, 4);

    for (v = 1; v <= 6; v++) {/* 1..4 快路径，5、6 溢出层 */
        i32_fsqu_push(&q, &v);
    }
    for (v = 1; v <= 4; v++) {/* 排空快路径，mpq 回到 enq == deq */
        CuAssertTrue(tc, ERR_OK == i32_fsqu_pop(&q, &out) && out == v);
    }
    CuAssertTrue(tc, ERR_OK == i32_fsqu_pop(&q, &out) && 5 == out);
    CuAssertTrue(tc, ERR_OK == i32_fsqu_pop(&q, &out) && 6 == out);
    i32_fsqu_free(&q);

    /* 批量路径同样：快路径与溢出层在一次 pop_batch 里按序拼齐 */
    i32_fsqu_init(&q, 2);
    for (v = 1; v <= 4; v++) {
        i32_fsqu_push(&q, &v);
    }
    n = i32_fsqu_pop_batch(&q, batch, 4);
    CuAssertTrue(tc, 4 == n);
    for (i = 0; i < 4; i++) {
        CuAssertTrue(tc, batch[i] == (int32_t)i + 1);
    }
    i32_fsqu_free(&q);
}

/* 单生产者顺序压测（说明见 _FSQU_ORDER_CHECK）：默认后端一遍，两个无锁环后端各一遍 */
static void test_fsqu_order_single_producer(CuTest *tc) {
    i32_fsqu_order_check(tc);
}
static void test_fsqu_ring_order(CuTest *tc) {
    i32_rfm_order_check(tc);
    i32_rfb_order_check(tc);
}

/* 无锁环后端的核心断言，两个后端 × 两个出队族 */
static void test_fsqu_ring_backends(CuTest *tc) {
    i32_rfm_core_check(tc, 0);
    i32_rfm_core_check(tc, 1);
    i32_rfb_core_check(tc, 0);
    i32_rfb_core_check(tc, 1);
}

/* trypush 语义不受溢出层影响：满时仍返 ERR_FAILED 且不落溢出层
   （pool / log 依赖这个丢弃语义，被污染会让它们变成无界增长） */
static void test_fsqu_trypush_no_overflow(CuTest *tc) {
    i32_fsqu q;
    int32_t v, out;
    uint32_t i;
    i32_fsqu_init(&q, 8);

    for (v = 1; v <= 8; v++) {
        CuAssertTrue(tc, ERR_OK == i32_fsqu_trypush(&q, &v));
    }
    for (i = 0; i < 4; i++) {/* 满后连续 trypush 全失败且 size 不增 */
        v = 100;
        CuAssertTrue(tc, ERR_FAILED == i32_fsqu_trypush(&q, &v));
        CuAssertTrue(tc, 8 == i32_fsqu_size(&q));
    }
    for (v = 1; v <= 8; v++) {/* 队列内仍只有最初 8 个 */
        CuAssertTrue(tc, ERR_OK == i32_fsqu_pop(&q, &out) && out == v);
    }
    CuAssertTrue(tc, ERR_FAILED == i32_fsqu_pop(&q, &out));
    i32_fsqu_free(&q);
}

/* 从未溢出的队列 free：溢出层延迟分配，ptr 恒为 NULL，依赖 FREE 宏的空指针守卫。
   声明的意图（"没分配过就不该释放"）要有观测点，否则 free 里多调一次 FREE 也看不出来。
   申请块数按 init 实测，不写死：快路径换实现块数就变（mpq 1 块、bbq 块头与数据分开 2 块） */
static void test_fsqu_never_overflow_free(CuTest *tc) {
    i32_fsqu q;
    int32_t v = 1, out;
    uint64_t a0, f0, a1, f1, ninit;
    mem_stat(&a0, &f0);
    i32_fsqu_init(&q, 8);
    mem_stat(&a1, &f1);
    ninit = a1 - a0;/* init 申请几块随 FSQU_FAST_MODEL 走,别写死 */
    i32_fsqu_push(&q, &v);
    CuAssertTrue(tc, ERR_OK == i32_fsqu_pop(&q, &out) && 1 == out);
    CuAssertTrue(tc, 0 == i32_fsqu_size(&q));
    mem_stat(&a0, &f0);
    i32_fsqu_free(&q);
    mem_stat(&a1, &f1);
    /* 释放数必须与 init 的申请数相等：溢出层从未分配过，多释放一次就是拿 NULL 之外的野指针去 free */
    CuAssertTrue(tc, 0 == a1 - a0);
    CuAssertTrue(tc, ninit == f1 - f0);
}

/* =======================================================================
 * chan —— 多生产/多消费 buffered chan 并发回归
 * f0a94c5 修复：_buffered_chan_recv 拷贝 msg->data/lens 到栈再 unlock，
 * 防止满载循环槽位被 push 覆盖；本用例用紧 buffer + 多 PC 校验消息无丢失/重复/串扰。
 * ======================================================================= */
static atomic_t _chan_race_consumed;
static int32_t _chan_race_recv[_CHAN_RACE_PRODS][_CHAN_RACE_PER_PROD];
static mutex_ctx _chan_race_mu;

static void _chan_race_producer(void *arg) {
    _chan_race_prod_arg *p = (_chan_race_prod_arg *)arg;
    _chan_race_msg msg;
    int32_t i;
    for (i = 0; i < _CHAN_RACE_PER_PROD; i++) {
        ZERO(&msg, sizeof(msg));
        msg.pid = p->pid;
        msg.seq = i;
        memset(msg.padding, 0xAB, sizeof(msg.padding));
        chan_send(p->chan, &msg, sizeof(msg), 1);// copy=1，chan 持有 heap 副本
    }
}

static void _chan_race_consumer(void *arg) {
    chan_ctx *chan = (chan_ctx *)arg;
    size_t lens;
    _chan_race_msg *msg;
    int32_t i;
    while (1) {
        msg = (_chan_race_msg *)chan_recv(chan, &lens);
        if (NULL == msg) {
            // chan_close 后 recv 返回 NULL，consumer 退出
            break;
        }
        if (lens == sizeof(_chan_race_msg)
            && msg->pid >= 0 && msg->pid < _CHAN_RACE_PRODS
            && msg->seq >= 0 && msg->seq < _CHAN_RACE_PER_PROD) {
            mutex_lock(&_chan_race_mu);
            _chan_race_recv[msg->pid][msg->seq]++;
            mutex_unlock(&_chan_race_mu);
            // 同时验证 padding 没被串扰
            for (i = 0; i < (int32_t)sizeof(msg->padding); i++) {
                if ((unsigned char)0xAB != (unsigned char)msg->padding[i]) {
                    mutex_lock(&_chan_race_mu);
                    _chan_race_recv[msg->pid][msg->seq] = -1;
                    mutex_unlock(&_chan_race_mu);
                    break;
                }
            }
        }
        FREE(msg);
        ATOMIC_ADD(&_chan_race_consumed, 1);
    }
}

static void test_chan_buffered_race(CuTest *tc) {
    chan_ctx *chan = chan_init(_CHAN_RACE_CAP);
    CuAssertPtrNotNull(tc, chan);

    ZERO(_chan_race_recv, sizeof(_chan_race_recv));
    mutex_init(&_chan_race_mu);
    ATOMIC_SET(&_chan_race_consumed, 0);

    pthread_t conss[_CHAN_RACE_CONSS];
    pthread_t prods[_CHAN_RACE_PRODS];
    _chan_race_prod_arg pargs[_CHAN_RACE_PRODS];

    int32_t i;
    // 先消费者，避免 producer 满载阻塞太久
    for (i = 0; i < _CHAN_RACE_CONSS; i++) {
        conss[i] = thread_creat(_chan_race_consumer, chan);
    }
    for (i = 0; i < _CHAN_RACE_PRODS; i++) {
        pargs[i].chan = chan;
        pargs[i].pid = i;
        prods[i] = thread_creat(_chan_race_producer, &pargs[i]);
    }
    for (i = 0; i < _CHAN_RACE_PRODS; i++) {
        thread_join(prods[i]);
    }
    // 等 consumer 把全部消息取走；到点就往下走，让末尾的数量断言报失败而不是挂死
    const uint64_t maxms = 30000;// 兜底：chan 丢消息要失败，不能挂死
    uint64_t deadline = nowms() + maxms;
    while ((int32_t)ATOMIC_GET(&_chan_race_consumed) < _CHAN_RACE_TOTAL
        && nowms() < deadline) {
        CPU_PAUSE();
    }
    chan_close(chan);
    for (i = 0; i < _CHAN_RACE_CONSS; i++) {
        thread_join(conss[i]);
    }

    CuAssertTrue(tc, _CHAN_RACE_TOTAL == (int32_t)ATOMIC_GET(&_chan_race_consumed));
    // 每个 (pid, seq) 应正好被收到一次；race 会表现为重复/丢失/-1（padding 串扰）
    int32_t bad = 0;
    int32_t p, s;
    for (p = 0; p < _CHAN_RACE_PRODS; p++) {
        for (s = 0; s < _CHAN_RACE_PER_PROD; s++) {
            if (1 != _chan_race_recv[p][s]) {
                bad++;
            }
        }
    }
    CuAssertTrue(tc, 0 == bad);
    mutex_free(&_chan_race_mu);
    chan_free(chan);
}

/* =======================================================================
 * hashmap
 * ======================================================================= */

static void test_hashmap(CuTest *tc) {
    kv_map *map = kv_map_new(0, NULL);
    CuAssertPtrNotNull(tc, map);
    CuAssertTrue(tc, 0 == kv_map_size(map));

    /* 插入 100 条记录 */
    for (int i = 0; i < 100; i++) {
        _kv kv;
        SNPRINTF(kv.key, sizeof(kv.key), "key_%d", i);
        kv.val = i * 10;
        kv_map_set(map, &kv);
    }
    CuAssertTrue(tc, 100 == (int)kv_map_size(map));

    /* 查找验证 */
    for (int i = 0; i < 100; i++) {
        _kv lookup;
        SNPRINTF(lookup.key, sizeof(lookup.key), "key_%d", i);
        const _kv *found = kv_map_get(map, &lookup);
        CuAssertPtrNotNull(tc, found);
        CuAssertTrue(tc, i * 10 == found->val);
    }

    /* 查找不存在的 key */
    _kv miss;
    SNPRINTF(miss.key, sizeof(miss.key), "not_exist");
    CuAssertTrue(tc, NULL == kv_map_get(map, &miss));

    /* 删除，数量减少 */
    _kv del;
    SNPRINTF(del.key, sizeof(del.key), "key_0");
    kv_map_delete(map, &del);
    CuAssertTrue(tc, 99 == (int)kv_map_size(map));
    CuAssertTrue(tc, NULL == kv_map_get(map, &del));

    /* 更新：同 key 再次 set 覆盖旧值 */
    _kv upd;
    SNPRINTF(upd.key, sizeof(upd.key), "key_1");
    upd.val = 9999;
    kv_map_set(map, &upd);
    const _kv *got = kv_map_get(map, &upd);
    CuAssertTrue(tc, 9999 == got->val);
    CuAssertTrue(tc, 99 == (int)kv_map_size(map));

    kv_map_free(map);
}

/* =======================================================================
 * heap —— 最小堆（compare 返回非零表示 lhs 优先于 rhs）
 * ======================================================================= */

static void test_heap(CuTest *tc) {
    _theap h;
    _theap_init(&h, 0);
    CuAssertTrue(tc, 0 == _theap_size(&h));
    CuAssertTrue(tc, 0 == _theap_capacity(&h));/* 延迟分配：init(0) 不申请 */

    /* 无序插入 5 个节点 */
    int vals[] = { 30, 10, 50, 20, 40 };
    _hnode nodes[5];
    for (int i = 0; i < 5; i++) {
        nodes[i].val = vals[i];
        _theap_insert(&h, &nodes[i]);
    }
    CuAssertTrue(tc, 5 == _theap_size(&h));

    /* 堆顶始终是最小值 */
    CuAssertTrue(tc, 10 == _theap_min(&h)->val);

    /* 逐个出堆，顺序应为升序 */
    int expected[] = { 10, 20, 30, 40, 50 };
    for (int i = 0; i < 5; i++) {
        CuAssertTrue(tc, expected[i] == _theap_min(&h)->val);
        _theap_dequeue(&h);
    }
    CuAssertTrue(tc, 0 == _theap_size(&h));

    /* 插入后随机删除中间节点 */
    for (int i = 0; i < 5; i++) {
        nodes[i].val = vals[i];
        _theap_insert(&h, &nodes[i]);
    }
    /* 删除值为 20 的节点（nodes[3]）*/
    _theap_remove(&h, &nodes[3]);
    CuAssertTrue(tc, 4 == _theap_size(&h));
    /* 堆顶仍是 10 */
    CuAssertTrue(tc, 10 == _theap_min(&h)->val);
    _theap_free(&h);
    CuAssertTrue(tc, 0 == _theap_capacity(&h));

    /* free 后再 insert 要能重新申请（同 ARR/QUE）；以前 maxsize 0 乘 2 仍是 0，会往 NULL 上写 */
    nodes[0].val = 7;
    _theap_insert(&h, &nodes[0]);
    CuAssertTrue(tc, 1 == _theap_size(&h) && HEAP_INIT_SIZE == _theap_capacity(&h));
    CuAssertTrue(tc, 7 == _theap_min(&h)->val);
    _theap_free(&h);

    /* 装满再多一个触发倍增 */
    _hnode grow[HEAP_INIT_SIZE + 1];
    _theap_init(&h, 0);
    for (int i = 0; i <= HEAP_INIT_SIZE; i++) {
        grow[i].val = HEAP_INIT_SIZE - i;
        _theap_insert(&h, &grow[i]);
    }
    CuAssertTrue(tc, 2 * HEAP_INIT_SIZE == _theap_capacity(&h));
    CuAssertTrue(tc, 0 == _theap_min(&h)->val);
    _theap_free(&h);
}

/* _theap_remove 删根路径 + 多次按引用删除后堆序保持（现有 test_heap 仅删中间节点）*/
static void test_heap_remove_root(CuTest *tc) {
    _theap h;
    _theap_init(&h, 0);
    int vals[] = { 50, 20, 70, 10, 60, 30, 90, 40, 80, 5 };
    int n = 10;
    _hnode nodes[10];
    int prev, top, i;

    /* 反复显式删除 root → 应按升序取出（覆盖 remove 删根 + 末节点替换 sift-down 深路径）*/
    for (i = 0; i < n; i++) {
        nodes[i].val = vals[i];
        _theap_insert(&h, &nodes[i]);
    }
    CuAssertTrue(tc, n == (int)_theap_size(&h));
    prev = -1;
    for (i = 0; i < n; i++) {
        top = _theap_min(&h)->val;
        CuAssertTrue(tc, top > prev); /* 值互异,严格升序 */
        prev = top;
        _theap_remove(&h, _theap_min(&h));
    }
    CuAssertTrue(tc, 0 == _theap_size(&h));

    /* 删到空之后直接复用同一批节点重新插入 */
    nodes[0].val = 1;
    nodes[1].val = 2;
    _theap_insert(&h, &nodes[0]);
    _theap_insert(&h, &nodes[1]);
    _theap_remove(&h, &nodes[1]);
    _theap_remove(&h, &nodes[0]);
    CuAssertTrue(tc, 0 == _theap_size(&h));
    CuAssertTrue(tc, _theap_empty(&h));
    /* 不重新 ZERO 直接复用，堆序仍正确（hidx 由 insert 自己写全，无需预清零）*/
    _theap_insert(&h, &nodes[0]);
    _theap_insert(&h, &nodes[1]);
    CuAssertTrue(tc, 1 == _theap_min(&h)->val);
    _theap_remove(&h, _theap_min(&h));
    _theap_remove(&h, _theap_min(&h));
    CuAssertTrue(tc, 0 == _theap_size(&h));

    /* 按引用删除若干内部/末/根节点后,余下仍保持堆序（覆盖 remove 触发的 sift 两方向）*/
    for (i = 0; i < n; i++) {
        nodes[i].val = vals[i];
        _theap_insert(&h, &nodes[i]);
    }
    _theap_remove(&h, &nodes[6]); /* 90 最大 */
    _theap_remove(&h, &nodes[3]); /* 10 最小(当前根) */
    _theap_remove(&h, &nodes[9]); /* 5(末插入) */
    CuAssertTrue(tc, (n - 3) == (int)_theap_size(&h));
    prev = -1;
    while (_theap_size(&h) > 0) {
        top = _theap_min(&h)->val;
        CuAssertTrue(tc, top > prev);
        prev = top;
        _theap_dequeue(&h);
    }
    CuAssertTrue(tc, 0 == _theap_size(&h));
    _theap_free(&h);
}

/* _theap_remove 删中间节点、末位替补比新父节点还小时必须上浮，只下沉会留下父大于子。
   按 1,10,2,11,12,3,4 插入，数组恰为插入序；删 11（下标 3），末位 4 顶进来，父节点是 10 */
static void test_heap_remove_sift_up(CuTest *tc) {
    _theap h;
    _hnode nodes[7];
    int vals[] = { 1, 10, 2, 11, 12, 3, 4 };
    int expect[] = { 1, 2, 3, 4, 10, 12 };
    uint32_t i;
    _theap_init(&h, 0);
    for (i = 0; i < 7; i++) {
        nodes[i].val = vals[i];
        _theap_insert(&h, &nodes[i]);
    }
    for (i = 0; i < 7; i++) {
        CuAssertTrue(tc, i == nodes[i].hidx);/* 布局如上，下面的下标断言才成立 */
    }
    _theap_remove(&h, &nodes[3]);
    CuAssertTrue(tc, 1 == nodes[6].hidx);/* 4 上浮到 10 原来的位置 */
    CuAssertTrue(tc, 3 == nodes[1].hidx);/* 10 被换下来 */
    for (i = 0; i < _theap_size(&h); i++) {
        CuAssertTrue(tc, i == h.p[i]->hidx);/* 回指下标自洽 */
        if (i > 0) {
            CuAssertTrue(tc, h.p[(i - 1) / 2]->val <= h.p[i]->val);/* 父不大于子 */
        }
    }
    for (i = 0; i < 6; i++) {
        CuAssertTrue(tc, expect[i] == _theap_min(&h)->val);
        _theap_dequeue(&h);
    }
    CuAssertTrue(tc, 0 == _theap_size(&h));
    _theap_free(&h);
}

/* =======================================================================
 * slist —— 侵入式双向链表（intrusive）
 * ======================================================================= */

/* 校验双向完整性 + 顺序：size、空判、正向序列==exp、反向序列==exp 逆序、head->prev/tail->next 边界 */
static void _slist_check(CuTest *tc, list_ctx *l, const int *exp, uint32_t n) {
    list_node *it;
    uint32_t cnt;
    CuAssertTrue(tc, n == list_size(l));
    if (0 == n) {
        CuAssertTrue(tc, 0 != list_empty(l));
        CuAssertTrue(tc, NULL == l->head);
        CuAssertTrue(tc, NULL == l->tail);
    } else {
        CuAssertTrue(tc, 0 == list_empty(l));
        CuAssertTrue(tc, NULL == l->head->prev);
        CuAssertTrue(tc, NULL == l->tail->next);
    }
    cnt = 0;
    for (it = l->head; NULL != it; it = it->next) {
        CuAssertTrue(tc, exp[cnt] == UPCAST(it, _lnode, node)->val);
        cnt++;
    }
    CuAssertTrue(tc, n == cnt);
    cnt = 0;
    for (it = l->tail; NULL != it; it = it->prev) {
        CuAssertTrue(tc, exp[n - 1 - cnt] == UPCAST(it, _lnode, node)->val);
        cnt++;
    }
    CuAssertTrue(tc, n == cnt);
}

/* 空表 / push_head·tail 顺序 / pop_head·tail / size */
static void test_slist_basic(CuTest *tc) {
    list_ctx l;
    _lnode nodes[5];
    int exp[5];
    int i;
    list_node *p;
    list_init(&l);
    CuAssertTrue(tc, 0 != list_empty(&l));
    CuAssertTrue(tc, 0 == list_size(&l));
    CuAssertTrue(tc, NULL == list_pop_head(&l));
    CuAssertTrue(tc, NULL == list_pop_tail(&l));
    /* push_tail → 正序 0..4 */
    for (i = 0; i < 5; i++) {
        nodes[i].val = i;
        list_push_tail(&l, &nodes[i].node);
        exp[i] = i;
    }
    _slist_check(tc, &l, exp, 5);
    /* pop_head FIFO：0,1,2,3,4 */
    for (i = 0; i < 5; i++) {
        p = list_pop_head(&l);
        CuAssertTrue(tc, NULL != p);
        CuAssertTrue(tc, i == UPCAST(p, _lnode, node)->val);
    }
    CuAssertTrue(tc, 0 != list_empty(&l));
    /* push_head → 逆序 4..0；pop_tail 取 0,1,2,3,4 */
    for (i = 0; i < 5; i++) {
        nodes[i].val = i;
        list_push_head(&l, &nodes[i].node);
    }
    for (i = 0; i < 5; i++) {
        exp[i] = 4 - i;
    }
    _slist_check(tc, &l, exp, 5);
    for (i = 0; i < 5; i++) {
        p = list_pop_tail(&l);
        CuAssertTrue(tc, NULL != p);
        CuAssertTrue(tc, i == UPCAST(p, _lnode, node)->val);
    }
    CuAssertTrue(tc, 0 == list_size(&l));
}

/* insert_before(含新头) / insert_after(含新尾) / 中间插 */
static void test_slist_insert(CuTest *tc) {
    list_ctx l;
    _lnode n[5];
    int exp[5];
    int i;
    list_init(&l);
    for (i = 0; i < 5; i++) {
        n[i].val = i;
    }
    list_push_tail(&l, &n[2].node);/* [2] */
    list_insert_before(&l, &n[2].node, &n[1].node);/* [1,2] */
    list_insert_before(&l, &n[1].node, &n[0].node);/* [0,1,2] n0 成新头 */
    CuAssertTrue(tc, &n[0].node == l.head);
    list_insert_after(&l, &n[2].node, &n[3].node);/* [0,1,2,3] */
    list_insert_after(&l, &n[3].node, &n[4].node);/* [0,1,2,3,4] n4 成新尾 */
    CuAssertTrue(tc, &n[4].node == l.tail);
    for (i = 0; i < 5; i++) {
        exp[i] = i;
    }
    _slist_check(tc, &l, exp, 5);
}

/* remove 头/尾/中/唯一 四位置 + 解后置 NULL + 复用 */
static void test_slist_remove(CuTest *tc) {
    list_ctx l;
    _lnode n[5];
    int exp[5];
    int i;
    list_init(&l);
    for (i = 0; i < 5; i++) {
        n[i].val = i;
        list_push_tail(&l, &n[i].node);
    }
    /* 中间 */
    list_remove(&l, &n[2].node);
    CuAssertTrue(tc, NULL == n[2].node.next);
    CuAssertTrue(tc, NULL == n[2].node.prev);
    exp[0] = 0; exp[1] = 1; exp[2] = 3; exp[3] = 4;
    _slist_check(tc, &l, exp, 4);
    /* 头 */
    list_remove(&l, &n[0].node);
    exp[0] = 1; exp[1] = 3; exp[2] = 4;
    _slist_check(tc, &l, exp, 3);
    /* 尾 */
    list_remove(&l, &n[4].node);
    exp[0] = 1; exp[1] = 3;
    _slist_check(tc, &l, exp, 2);
    /* 至唯一 → 空 */
    list_remove(&l, &n[1].node);
    exp[0] = 3;
    _slist_check(tc, &l, exp, 1);
    list_remove(&l, &n[3].node);
    _slist_check(tc, &l, NULL, 0);
    /* 复用被删节点 */
    list_push_head(&l, &n[2].node);
    list_push_tail(&l, &n[0].node);
    exp[0] = 2; exp[1] = 0;
    _slist_check(tc, &l, exp, 2);
}

/* splice(空src/空dst/两端非空) + list_iter + foreach + foreach_safe(遍历中删) */
static void test_slist_splice_iter(CuTest *tc) {
    list_ctx a, b;
    _lnode na[3], nb[3];
    int exp[6];
    int i, cnt;
    list_node *p;
    list_iter it;
    /* 空 src → no-op */
    list_init(&a);
    list_init(&b);
    for (i = 0; i < 3; i++) {
        na[i].val = i;
        list_push_tail(&a, &na[i].node);
    }
    list_splice_tail(&a, &b);
    exp[0] = 0; exp[1] = 1; exp[2] = 2;
    _slist_check(tc, &a, exp, 3);
    /* 空 dst ← src（src 清空）*/
    list_init(&a);
    list_init(&b);
    for (i = 0; i < 3; i++) {
        nb[i].val = 10 + i;
        list_push_tail(&b, &nb[i].node);
    }
    list_splice_tail(&a, &b);
    exp[0] = 10; exp[1] = 11; exp[2] = 12;
    _slist_check(tc, &a, exp, 3);
    _slist_check(tc, &b, NULL, 0);
    /* 两端非空：顺序保留 + size 相加 */
    list_init(&a);
    list_init(&b);
    for (i = 0; i < 3; i++) {
        na[i].val = i;
        list_push_tail(&a, &na[i].node);
    }
    for (i = 0; i < 3; i++) {
        nb[i].val = 10 + i;
        list_push_tail(&b, &nb[i].node);
    }
    list_splice_tail(&a, &b);
    exp[0] = 0; exp[1] = 1; exp[2] = 2; exp[3] = 10; exp[4] = 11; exp[5] = 12;
    _slist_check(tc, &a, exp, 6);
    _slist_check(tc, &b, NULL, 0);
    /* list_iter 顺序遍历 */
    list_iter_init(&it, &a);
    cnt = 0;
    while (NULL != (p = list_iter_next(&it))) {
        CuAssertTrue(tc, exp[cnt] == UPCAST(p, _lnode, node)->val);
        cnt++;
    }
    CuAssertTrue(tc, 6 == cnt);
    /* foreach 顺序遍历 */
    cnt = 0;
    list_foreach(&a, fit) {
        CuAssertTrue(tc, exp[cnt] == UPCAST(fit, _lnode, node)->val);
        cnt++;
    }
    CuAssertTrue(tc, 6 == cnt);
    /* foreach_safe 遍历中删偶数值（剩 1,11）*/
    list_foreach_safe(&a, sit, tmp) {
        if (0 == (UPCAST(sit, _lnode, node)->val & 1)) {
            list_remove(&a, sit);
        }
    }
    exp[0] = 1; exp[1] = 11;
    _slist_check(tc, &a, exp, 2);
    /* 空表遍历：list_iter / foreach / foreach_safe body 均执行 0 次 */
    list_init(&b);
    cnt = 0;
    list_iter_init(&it, &b);
    while (NULL != list_iter_next(&it)) {
        cnt++;
    }
    CuAssertTrue(tc, 0 == cnt);
    cnt = 0;
    list_foreach(&b, eit) {
        cnt++;
    }
    CuAssertTrue(tc, 0 == cnt);
    cnt = 0;
    list_foreach_safe(&b, esit, etmp) {
        cnt++;
    }
    CuAssertTrue(tc, 0 == cnt);
}

/* =======================================================================
 * i_que —— 环形队列（自动扩容）
 * ======================================================================= */

// maxsize 为 0 是"延迟分配"不是"零容量"：i_que_full 报未满，
// 于是 i_que_trypush 也能触发首次分配，两种入队方式不再互斥
static void test_queue_lazy_trypush(CuTest *tc) {
    i_que q;
    i_que_init(&q, 0);
    CuAssertTrue(tc, 0 == i_que_capacity(&q));
    CuAssertTrue(tc, !i_que_full(&q));

    int v = 7;
    CuAssertIntEquals(tc, ERR_OK, i_que_trypush(&q, &v));
    CuAssertTrue(tc, 1 == i_que_size(&q));
    CuAssertTrue(tc, i_que_capacity(&q) > 0);
    CuAssertTrue(tc, 7 == *(int *)i_que_pop(&q));

    /* 分配之后照常按容量判满。容量先取出来：写在循环条件里的话每轮都重算一次，
       而 trypush 撑不满时 maxsize 会翻倍，循环条件跟着变，这圈就永远走不完 */
    const int cap = (int)i_que_capacity(&q);
    for (int i = 0; i < cap; i++) {
        CuAssertIntEquals(tc, ERR_OK, i_que_trypush(&q, &i));
    }
    CuAssertTrue(tc, i_que_full(&q));
    CuAssertIntEquals(tc, ERR_FAILED, i_que_trypush(&q, &v));
    i_que_free(&q);
}

static void test_queue(CuTest *tc) {
    i_que q;
    i_que_init(&q, 4);

    CuAssertTrue(tc, 0 == i_que_size(&q));
    CuAssertTrue(tc, 4 == i_que_capacity(&q));
    CuAssertTrue(tc, i_que_empty(&q));
    CuAssertTrue(tc, NULL == i_que_pop(&q));
    CuAssertTrue(tc, NULL == i_que_peek(&q));

    /* 入队 5 个，超出初始容量后自动扩容 */
    for (int i = 0; i < 5; i++) {
        i_que_push(&q, &i);
    }
    CuAssertTrue(tc, 5 == (int)i_que_size(&q));

    /* peek 不改变队列大小 */
    CuAssertTrue(tc, 0 == *(int *)i_que_peek(&q));
    CuAssertTrue(tc, 5 == (int)i_que_size(&q));

    /* FIFO 顺序出队 */
    for (int i = 0; i < 5; i++) {
        CuAssertTrue(tc, i == *(int *)i_que_pop(&q));
    }
    CuAssertTrue(tc, i_que_empty(&q));

    /* at() 访问 */
    for (int i = 0; i < 8; i++) {
        i_que_push(&q, &i);
    }
    CuAssertTrue(tc, 0 == *(int *)i_que_at(&q, 0));
    CuAssertTrue(tc, 7 == *(int *)i_que_at(&q, 7));
    CuAssertTrue(tc, NULL == i_que_at(&q, 8));

    /* clear 后队列为空 */
    i_que_clear(&q);
    CuAssertTrue(tc, i_que_empty(&q));

    i_que_free(&q);
}

/* u32_que_free / iarr_free 必须把长度字段一并复位。只置空 ptr 会留下 ptr==NULL 但
 * size < maxsize 的不一致态：再 push 时 size != maxsize 不触发 resize，
 * 直接往 (char*)NULL + off 写。参照 binary_free 的写法 */
static void test_queue_array_free_resets(CuTest *tc) {
    u32_que q;
    iarr a;
    uint32_t v;
    int32_t i;
    int iv;

    u32_que_init(&q, 4);
    for (i = 0; i < 5; i++) {
        v = (uint32_t)i;
        u32_que_push(&q, &v);
    }
    CuAssertTrue(tc, q.size > 0 && q.maxsize > q.size);/* 触发过一次扩容 */
    u32_que_free(&q);
    CuAssertTrue(tc, NULL == q.ptr);
    CuAssertTrue(tc, 0 == q.size);
    CuAssertTrue(tc, 0 == q.maxsize);
    CuAssertTrue(tc, 0 == q.offset);

    iarr_init(&a, 4);
    for (i = 0; i < 5; i++) {
        v = (uint32_t)i;
        iv = (int)v;
        iarr_push_back(&a, &iv);
    }
    CuAssertTrue(tc, a.size > 0 && a.maxsize > a.size);
    iarr_free(&a);
    CuAssertTrue(tc, NULL == a.ptr);
    CuAssertTrue(tc, 0 == a.size);
    CuAssertTrue(tc, 0 == a.maxsize);
}

/* =======================================================================
 * ARR_DECL —— 动态数组（按值定长元素）
 * ======================================================================= */

// 以 int 元素覆盖核心 API：init/push_back/扩容/swap/add/del/pop_back/del_nomove/clear
static void test_array(CuTest *tc) {
    iarr a;
    iarr_init(&a, 4);

    CuAssertTrue(tc, 0 == iarr_size(&a));
    CuAssertTrue(tc, 4 == a.maxsize);
    CuAssertTrue(tc, iarr_empty(&a));
    CuAssertTrue(tc, NULL == iarr_front(&a));
    CuAssertTrue(tc, NULL == iarr_back(&a));

    /* push_back 超出容量后自动扩容 */
    for (int i = 1; i <= 8; i++) {
        iarr_push_back(&a, &i);
    }
    CuAssertTrue(tc, 8 == (int)iarr_size(&a));
    CuAssertTrue(tc, 1 == *iarr_front(&a));
    CuAssertTrue(tc, 8 == *iarr_back(&a));
    CuAssertTrue(tc, 3 == *iarr_at(&a, 2));

    /* 交换 */
    iarr_swap(&a, 0, 7);
    CuAssertTrue(tc, 8 == *iarr_front(&a));
    CuAssertTrue(tc, 1 == *iarr_back(&a));
    iarr_swap(&a, 0, 7);

    /* 在指定位置插入 */
    int v = 99;
    iarr_add(&a, &v, 2);
    CuAssertTrue(tc, 99 == *iarr_at(&a, 2));
    CuAssertTrue(tc, 9 == (int)iarr_size(&a));

    /* 删除（保持顺序）*/
    iarr_del(&a, 2);
    CuAssertTrue(tc, 3 == *iarr_at(&a, 2));
    CuAssertTrue(tc, 8 == (int)iarr_size(&a));

    /* pop_back */
    CuAssertTrue(tc, 8 == *iarr_pop_back(&a));
    CuAssertTrue(tc, 7 == (int)iarr_size(&a));

    /* del_nomove：用末尾元素填充被删位置 */
    iarr_del_nomove(&a, 0);
    CuAssertTrue(tc, 7 == *iarr_front(&a));/* 末尾元素移到首位 */
    CuAssertTrue(tc, 6 == (int)iarr_size(&a));

    /* clear 不释放内存 */
    iarr_clear(&a);
    CuAssertTrue(tc, iarr_empty(&a));
    CuAssertTrue(tc, NULL == iarr_pop_back(&a));

    iarr_free(&a);
}

// 负下标的合法边界：-1 是末元素、-size 是首元素；add 的插入位允许等于 size（即追加），
// 负下标插在该元素之前。越界会 abort，进程内测不了，这里只覆盖各入口的合法端点
static void test_array_negpos(CuTest *tc) {
    iarr a;
    int i, v;
    iarr_init(&a, 4);
    for (i = 1; i <= 8; i++) {
        iarr_push_back(&a, &i);
    }
    CuAssertTrue(tc, 8 == *iarr_at(&a, -1));
    CuAssertTrue(tc, 7 == *iarr_at(&a, -2));
    CuAssertTrue(tc, 1 == *iarr_at(&a, -8));
    v = 99;
    iarr_add(&a, &v, -1);// [1..7, 99, 8]
    CuAssertTrue(tc, 9 == (int)iarr_size(&a));
    CuAssertTrue(tc, 99 == *iarr_at(&a, -2));
    CuAssertTrue(tc, 8 == *iarr_at(&a, -1));
    v = 100;
    iarr_add(&a, &v, (int32_t)iarr_size(&a));// 插入位等于 size：[1..7, 99, 8, 100]
    CuAssertTrue(tc, 100 == *iarr_at(&a, -1));
    iarr_swap(&a, 0, -1);// [100, 2..7, 99, 8, 1]
    CuAssertTrue(tc, 100 == *iarr_at(&a, 0));
    CuAssertTrue(tc, 1 == *iarr_at(&a, -1));
    iarr_del(&a, -1);// [100, 2..7, 99, 8]
    CuAssertTrue(tc, 8 == *iarr_at(&a, -1));
    iarr_del(&a, -(int32_t)iarr_size(&a));// 删首元素：[2..7, 99, 8]
    CuAssertTrue(tc, 2 == *iarr_at(&a, 0));
    iarr_del_nomove(&a, -1);// 删的就是末元素，不搬动：[2..7, 99]
    CuAssertTrue(tc, 99 == *iarr_at(&a, -1));
    CuAssertTrue(tc, 7 == (int)iarr_size(&a));
    iarr_free(&a);
}

// 以 void * 元素再过一遍，验证指针类型场景同样工作
static void test_array_ptr(CuTest *tc) {
    parr a;
    parr_init(&a, 4);

    CuAssertTrue(tc, 0 == parr_size(&a));
    CuAssertTrue(tc, parr_empty(&a));
    CuAssertTrue(tc, NULL == parr_front(&a));
    CuAssertTrue(tc, NULL == parr_back(&a));

    /* push_back 超出初始容量后自动扩容 */
    void *p1 = (void *)0x1111;
    void *p2 = (void *)0x2222;
    void *p3 = (void *)0x3333;
    void *p4 = (void *)0x4444;
    void *p5 = (void *)0x5555;
    parr_push_back(&a, &p1);
    parr_push_back(&a, &p2);
    parr_push_back(&a, &p3);
    parr_push_back(&a, &p4);
    parr_push_back(&a, &p5); /* 触发扩容 */

    CuAssertTrue(tc, 5 == parr_size(&a));
    CuAssertTrue(tc, p1 == *parr_front(&a));
    CuAssertTrue(tc, p5 == *parr_back(&a));
    CuAssertTrue(tc, p3 == *parr_at(&a, 2));

    /* pop_back */
    CuAssertTrue(tc, p5 == *parr_pop_back(&a));
    CuAssertTrue(tc, 4 == parr_size(&a));

    /* del_nomove：用末尾元素填充被删位置 */
    parr_del_nomove(&a, 0);
    CuAssertTrue(tc, p4 == *parr_front(&a));
    CuAssertTrue(tc, 3 == parr_size(&a));

    /* swap：交换两个位置（当前数组为 [p4, p2, p3]，交换 0 和 1 → [p2, p4, p3]）*/
    parr_swap(&a, 0, 1);
    CuAssertTrue(tc, p2 == *parr_front(&a));
    parr_swap(&a, 0, 1); /* 换回 [p4, p2, p3] */

    /* clear 后为空 */
    parr_clear(&a);
    CuAssertTrue(tc, parr_empty(&a));
    CuAssertTrue(tc, NULL == parr_pop_back(&a));

    parr_free(&a);
}

/* =======================================================================
 * hashmap —— scan / iter / clear
 * ======================================================================= */

static int _scan_sum;

static int32_t _scan_cb(const _kv *item, void *udata) {
    (void)udata;
    _scan_sum += item->val;
    return 1; /* 非 0=继续遍历 */
}

static int32_t _scan_stop_cb(const _kv *item, void *udata) {
    int *count = (int *)udata;
    (*count)++;
    (void)item;
    return (*count < 3); /* 访问 3 个后停止 */
}

/* 故意在回调里往被扫的表里插一条，验证 scan 能检出并中止 */
static int32_t _scan_insert_cb(const _kv *item, void *udata) {
    kv_map *map = (kv_map *)udata;
    (void)item;
    _kv kv;
    SNPRINTF(kv.key, sizeof(kv.key), "in_scan");
    kv.val = 100;
    kv_map_set(map, &kv);
    return 1;
}

static void test_hashmap_scan_iter(CuTest *tc) {
    kv_map *map = kv_map_new(0, NULL);
    /* 插入 5 条，val 为 1~5 */
    for (int i = 1; i <= 5; i++) {
        _kv kv;
        SNPRINTF(kv.key, sizeof(kv.key), "k%d", i);
        kv.val = i;
        kv_map_set(map, &kv);
    }
    CuAssertTrue(tc, 5 == (int)kv_map_size(map));

    /* scan：遍历所有元素，求和 = 1+2+3+4+5 = 15 */
    _scan_sum = 0;
    kv_map_scan(map, _scan_cb, NULL);
    CuAssertTrue(tc, 15 == _scan_sum);

    /* scan 提前终止：回调返回 false 时停止，访问计数为 3 */
    int count = 0;
    kv_map_scan(map, _scan_stop_cb, &count);
    CuAssertTrue(tc, 3 == count);

    /* 回调里增删被扫的表会让后续桶被跳过或重复访问，扩容更会换掉 buckets 数组；
       scan 须按 version 检出并中止，而不是拿着旧数组接着扫。
       用独立的表做，免得插进去的那条影响后面对 map 的计数断言 */
    {
        kv_map *m2 = kv_map_new(0, NULL);
        _kv seed;
        SNPRINTF(seed.key, sizeof(seed.key), "seed");
        seed.val = 1;
        kv_map_set(m2, &seed);
        CuAssertTrue(tc, !kv_map_scan(m2, _scan_insert_cb, m2));
        /* 不改表的回调照常走完 */
        _scan_sum = 0;
        CuAssertTrue(tc, kv_map_scan(m2, _scan_cb, NULL));
        kv_map_free(m2);
    }

    /* iter：用游标方式遍历所有元素 */
    size_t i = 0;
    _kv *item;
    int iter_count = 0;
    while (kv_map_iter(map, &i, &item)) {
        iter_count++;
    }
    CuAssertTrue(tc, 5 == iter_count);

    _kv mid;
    SNPRINTF(mid.key, sizeof(mid.key), "mid");
    mid.val = 42;
#if HASHMAP_ITER_VERSIONED
    /* 迭代途中新增：robin-hood 插入会把已有条目往后挪，被挪到游标之前的那个会被整个跳过，
       所以 kv_map_iter 必须能报出"被改过"。不涨 version 时它会一声不吭地漏元素 */
    i = 0;
    CuAssertTrue(tc, kv_map_iter(map, &i, &item));
    kv_map_set(map, &mid);
    CuAssertTrue(tc, !kv_map_iter(map, &i, &item));
#else
    /* 未开版本号时 kv_map_iter 不做失效检测（默认 32 位构建就是这样，见 hashmap.h），
       持游标插入在那里是 UB，故这里只插入、不带活游标 */
    kv_map_set(map, &mid);
#endif
    /* 游标归零后重新遍历一切正常，且新元素已在其中 */
    i = 0;
    iter_count = 0;
    while (kv_map_iter(map, &i, &item)) {
        iter_count++;
    }
    CuAssertTrue(tc, 6 == iter_count);

    /* 替换不挪位置，不该判为结构性改动 */
    i = 0;
    CuAssertTrue(tc, kv_map_iter(map, &i, &item));
    mid.val = 43;
    CuAssertTrue(tc, NULL != kv_map_set(map, &mid));// 返回被替换的旧值
    CuAssertTrue(tc, kv_map_iter(map, &i, &item));

    /* clear(false) 后计数为 0，仍可重新插入 */
    kv_map_clear(map, false);
    CuAssertTrue(tc, 0 == (int)kv_map_size(map));
    _kv kv;
    SNPRINTF(kv.key, sizeof(kv.key), "after_clear");
    kv.val = 999;
    kv_map_set(map, &kv);
    CuAssertTrue(tc, 1 == (int)kv_map_size(map));

    kv_map_free(map);
}

/* =======================================================================
 * hashmap 边界与高级 API：
 *   - kv_map_clear(true) 保留当前桶数、不分配（不是回缩）
 *   - kv_map_*_with_hash 预算哈希变体（调用方算好哈希后传入）
 *   - probe 直接位置探针访问
 *   - set_grow_by_power / set_load_factor 配置
 *   - oom 标志
 * ======================================================================= */
static void test_hashmap_with_hash_variants(CuTest *tc) {
    kv_map *map = kv_map_new(0, NULL);
    CuAssertPtrNotNull(tc, map);

    /* set_with_hash：调用方预算哈希后直接传入 */
    _kv kv;
    SNPRINTF(kv.key, sizeof(kv.key), "alpha");
    kv.val = 42;
    uint64_t h = hash(kv.key, strlen(kv.key));
    CuAssertTrue(tc, NULL == kv_map_set_with_hash(map, &kv, h));
    CuAssertTrue(tc, 1 == (int)kv_map_size(map));

    /* get_with_hash：用同一 hash 查回 */
    _kv lookup;
    SNPRINTF(lookup.key, sizeof(lookup.key), "alpha");
    uint64_t hl = hash(lookup.key, strlen(lookup.key));
    const _kv *got = kv_map_get_with_hash(map, &lookup, hl);
    CuAssertPtrNotNull(tc, got);
    CuAssertTrue(tc, 42 == got->val);

    /* delete_with_hash */
    const _kv *removed = kv_map_delete_with_hash(map, &lookup, hl);
    CuAssertPtrNotNull(tc, removed);
    CuAssertTrue(tc, 0 == (int)kv_map_size(map));

    /* oom 默认 0（无 OOM 发生） */
    CuAssertTrue(tc, !kv_map_oom(map));

    kv_map_free(map);
}

// clear 的 update_cap 语义（hashset 那侧透传同一参数），与名字直觉相反：
//   非 0 = 把 cap 抬到当前已增长的桶数，不做任何分配，峰值容量就此保留；
//   0    = 重新分配桶数组缩回建表 cap，一次 malloc + 一次 free。
// 这里只断言 nbuckets / cap;同尺寸重分配从桶数上看不出来,分配次数另见 test_hashmap_clear_allocs
static kv_map *_hm_new_filled(void) {
    kv_map *map = kv_map_new(16, NULL);
    _kv kv;
    int32_t i;
    for (i = 0; i < 256; i++) {
        SNPRINTF(kv.key, sizeof(kv.key), "k_%d", i);
        kv.val = i;
        kv_map_set(map, &kv);
    }
    return map;
}
static void test_hashmap_clear_cap(CuTest *tc) {
    kv_map *map = _hm_new_filled();
    size_t grown;
    CuAssertPtrNotNull(tc, map);
    grown = map->nbuckets;
    CuAssertTrue(tc, grown > map->cap);/* 灌了 256 条,确实扩过容 */

    /* clear(1):cap 抬到当前桶数,桶数组原样保留 */
    kv_map_clear(map, 1);
    CuAssertTrue(tc, 0 == (int)kv_map_size(map));
    CuAssertTrue(tc, grown == map->nbuckets);
    CuAssertTrue(tc, grown == map->cap);

    /* 上一步已把 cap 抬平,再 clear(0) 也没得缩 */
    kv_map_clear(map, 0);
    CuAssertTrue(tc, grown == map->nbuckets);
    CuAssertTrue(tc, grown == map->cap);
    kv_map_free(map);

    /* 直接 clear(0):桶数组缩回建表时的 cap */
    map = _hm_new_filled();
    CuAssertPtrNotNull(tc, map);
    CuAssertTrue(tc, 16 == map->cap);
    kv_map_clear(map, 0);
    CuAssertTrue(tc, 0 == (int)kv_map_size(map));
    CuAssertTrue(tc, 16 == map->nbuckets);
    kv_map_free(map);
}

// clear 的分配次数（hashmap.h 文件头写明的契约）：clear(1) 不分配；扩过容的表 clear(0)
// 恰好换一次桶数组，即一次分配 + 一次释放；已是建表大小时 clear(0) 也不分配。
// MEMORY_CHECK 关闭时 mem_stat 恒报 0，期望值跟着取 0
static void test_hashmap_clear_allocs(CuTest *tc) {
    kv_map *map;
    uint64_t a0, f0, a1, f1;
    uint64_t one = MEMORY_CHECK ? 1 : 0;

    map = _hm_new_filled();
    mem_stat(&a0, &f0);
    kv_map_clear(map, 1);
    mem_stat(&a1, &f1);
    CuAssertTrue(tc, a1 == a0 && f1 == f0);
    kv_map_free(map);

    map = _hm_new_filled();
    mem_stat(&a0, &f0);
    kv_map_clear(map, 0);
    mem_stat(&a1, &f1);
    CuAssertTrue(tc, one == a1 - a0 && one == f1 - f0);
    mem_stat(&a0, &f0);
    kv_map_clear(map, 0);
    mem_stat(&a1, &f1);
    CuAssertTrue(tc, a1 == a0 && f1 == f0);
    kv_map_free(map);
}

// name##_new 的容量取整：至少 16，否则向上取 2 的幂，cap 与 nbuckets 同值。
// set 覆盖 / delete 返回的副本放在表内备用槽，扩容不碰它：删一条后灌到扩容，副本仍是被删的那条
static void test_hashmap_new_round_and_spare(CuTest *tc) {
    const size_t caps[] = { 0, 1, 15, 16, 17, 1025 };
    const size_t want[] = { 16, 16, 16, 16, 32, 2048 };
    kv_map *map;
    _kv kv;
    const _kv *del;
    size_t k, nb;
    int32_t i;

    for (k = 0; k < sizeof(caps) / sizeof(caps[0]); k++) {
        map = kv_map_new(caps[k], NULL);
        CuAssertPtrNotNull(tc, map);
        CuAssertTrue(tc, want[k] == map->nbuckets && want[k] == map->cap);
        kv_map_free(map);
    }

    map = kv_map_new(16, NULL);
    CuAssertPtrNotNull(tc, map);
    SNPRINTF(kv.key, sizeof(kv.key), "victim");
    kv.val = -1;
    kv_map_set(map, &kv);
    del = kv_map_delete(map, &kv);
    CuAssertPtrNotNull(tc, del);
    nb = map->nbuckets;
    for (i = 0; i < 256; i++) {/* 全是新键，不走覆盖，不会写备用槽 */
        SNPRINTF(kv.key, sizeof(kv.key), "k_%d", i);
        kv.val = i;
        kv_map_set(map, &kv);
    }
    CuAssertTrue(tc, map->nbuckets > nb);/* 确实扩过容 */
    CuAssertStrEquals(tc, "victim", del->key);
    CuAssertIntEquals(tc, -1, del->val);
    kv_map_free(map);
}

// set_load_factor 设的负载因子必须在扩容后继续生效。
// 修复前 resize0 从"用默认 60 建出来的临时 map"拷 growat/shrinkat，自定义因子被悄悄换回默认，
// 而 map->loadfactor 字段仍报旧值——外部无从察觉。直接读 nbuckets/growat 观测
static void test_hashmap_load_factor_survives_resize(CuTest *tc) {
    kv_map *map = kv_map_new(16, NULL);
    _kv kv;
    int32_t i;
    CuAssertPtrNotNull(tc, map);
    kv_map_set_load_factor(map, 0.90);
    CuAssertTrue(tc, 16 == map->nbuckets);
    CuAssertTrue(tc, 14 == map->growat);/* 16*90/100 */

    /* 灌到触发一次扩容 */
    for (i = 0; i < 15; i++) {
        SNPRINTF(kv.key, sizeof(kv.key), "k_%d", i);
        kv.val = i;
        kv_map_set(map, &kv);
    }
    CuAssertTrue(tc, 15 == (int)kv_map_size(map));
    CuAssertTrue(tc, 32 == map->nbuckets);
    /* 扩容后 growat 按 90 重算成 28。修复前 resize 从"默认 60 建的临时表"拷阈值,
       这里会是 19,而 loadfactor 字段仍报 90 */
    CuAssertTrue(tc, 90 == map->loadfactor);
    CuAssertTrue(tc, 28 == map->growat);

    for (i = 15; i < 28; i++) {
        SNPRINTF(kv.key, sizeof(kv.key), "k_%d", i);
        kv.val = i;
        kv_map_set(map, &kv);
    }
    CuAssertTrue(tc, 28 == (int)kv_map_size(map));
    CuAssertTrue(tc, 32 == map->nbuckets);/* 到 growat 之前不扩 */

    SNPRINTF(kv.key, sizeof(kv.key), "k_28");
    kv.val = 28;
    kv_map_set(map, &kv);
    CuAssertTrue(tc, 64 == map->nbuckets);
    kv_map_free(map);
}

static void test_hashmap_clear_update_cap(CuTest *tc) {
    /* 插入触发扩容后 clear(true)：计数清零，桶数原样保留（容量语义见 test_hashmap_clear_cap） */
    kv_map *map = kv_map_new(16, NULL);
    CuAssertPtrNotNull(tc, map);

    /* 触发扩容：插入 256 条 */
    for (int i = 0; i < 256; i++) {
        _kv kv;
        SNPRINTF(kv.key, sizeof(kv.key), "k_%d", i);
        kv.val = i;
        kv_map_set(map, &kv);
    }
    CuAssertTrue(tc, 256 == (int)kv_map_size(map));

    /* clear(true) → 计数清零，桶数保持在已增长的规模 */
    kv_map_clear(map, true);
    CuAssertTrue(tc, 0 == (int)kv_map_size(map));

    /* 清空后仍可插入 */
    _kv k2;
    SNPRINTF(k2.key, sizeof(k2.key), "after_resize");
    k2.val = 1;
    kv_map_set(map, &k2);
    CuAssertTrue(tc, 1 == (int)kv_map_size(map));

    /* set_grow_by_power / set_load_factor：不应崩溃 */
    kv_map_set_grow_by_power(map, 2);
    kv_map_set_load_factor(map, 0.8);

    /* 再插入大量数据，扩容仍正常工作 */
    for (int i = 0; i < 100; i++) {
        _kv kv;
        SNPRINTF(kv.key, sizeof(kv.key), "extra_%d", i);
        kv.val = i;
        kv_map_set(map, &kv);
    }
    CuAssertTrue(tc, 101 == (int)kv_map_size(map));

    /* growpower 的下界钳位：power=0 不钳的话扩容时 new_cap = nbuckets << 0 == nbuckets,
       撞上 插入路径里 new_cap <= nbuckets 那道守卫 → oom → 插不进去、count 停住;
       钳到 1 才照常翻倍。growpower 现在读得到,这里仍按行为验,顺带覆盖插入路径的守卫。
       上界(>16 钳成 16)测不了：即便钳住,16 << 16 也要一百万个桶,单元测试不该分配那么多 */
    {
        kv_map *m0 = kv_map_new(16, NULL);
        CuAssertPtrNotNull(tc, m0);
        kv_map_set_grow_by_power(m0, 0);
        for (int i = 0; i < 256; i++) {
            _kv kv;
            SNPRINTF(kv.key, sizeof(kv.key), "p0_%d", i);
            kv.val = i;
            kv_map_set(m0, &kv);
        }
        CuAssertTrue(tc, !kv_map_oom(m0));
        CuAssertTrue(tc, 256 == (int)kv_map_size(m0));
        kv_map_free(m0);
    }

    /* probe：按 buckets 索引访问 */
    int probed = 0;
    for (size_t pos = 0; pos < 64 && probed < 5; pos++) {
        const _kv *item = kv_map_probe(map, pos);
        if (NULL != item) {
            probed++;
        }
    }
    CuAssertTrue(tc, probed > 0);

    kv_map_free(map);
}

/* ======================================================================= */

/* =======================================================================
 * hashmap 上游自检移植：原在 hashmap.c 的 #ifdef HASHMAP_TEST 块内，本工程从不编译该块，
 * 这些断言在本树上从未跑过。三处内部访问改写为公开 API 等价物：
 *   map->count     → up_imap_size
 *   deepcount(map) → up_imap_scan 计数（两者同为"活元素个数"）
 *   map->cap       → 已由 test_hashmap_clear_cap 直接读字段覆盖，不在此重复
 * 种子固定、PRNG 自带：上游以 time(NULL) 播种全局 rand()，会让失败不可复现并干扰其他用例
 * ======================================================================= */
static test_rng _up_rng;
static int32_t _up_fail_on;
static uintptr_t _up_allocs;
// 带失败注入的分配器,只用于测试自己分配字符串元素(表本身走项目分配器);_up_allocs 记未释放块数
static void *_up_malloc(size_t size) {
    void *mem;
    if (0 != _up_fail_on
        && 0 == test_rng_next(&_up_rng) % UP_FAIL_ODDS) {
        return NULL;
    }
    mem = malloc(sizeof(uintptr_t) + size);
    if (NULL == mem) {
        return NULL;
    }
    *(uintptr_t *)mem = size;
    _up_allocs++;
    return (char *)mem + sizeof(uintptr_t);
}
static void _up_free(void *ptr) {
    if (NULL != ptr) {
        _up_allocs--;
        free((char *)ptr - sizeof(uintptr_t));
    }
}
// scan / iter 回调：以元素的整数值为下标打标记，用来验证两者都访问到全部元素
static int32_t _up_mark(const int32_t *item, void *udata) {
    int32_t *marks = *(int32_t **)udata;
    marks[*item] = 1;
    return 1;
}
// scan 回调：只累计访问次数，替代需要读内部桶的 deepcount
static int32_t _up_tally(const int32_t *item, void *udata) {
    (void)item;
    (*(size_t *)udata)++;
    return 1;
}
static void _up_free_str(void *item) {
    _up_free(*(char **)item);
}
static size_t _up_live(up_imap *map) {
    size_t n = 0;
    up_imap_scan(map, _up_tally, &n);
    return n;
}

// 上游 all() 的整数主循环：每插入一个元素都校验 count 与活元素数、此前所有键仍可取、
// 重复 set 返回旧元素、delete 后不可再取；随后 scan 与 iter 须各自访问到全部元素；
// 最后逐个删除再校验一遍剩余键。表走项目分配器，set 遇 oom 重试是照搬上游的写法
static void test_hashmap_upstream_churn(CuTest *tc) {
    int32_t *vals;
    int32_t *marks;
    up_imap *map;
    const int32_t *v;
    size_t iter;
    int32_t *item;
    int32_t i, j;

    test_rng_init(&_up_rng, 88172645463325252ULL);
    MALLOC(vals, sizeof(int32_t) * UP_N);
    for (i = 0; i < UP_N; i++) {
        vals[i] = i;
    }
    test_shuffle(&_up_rng, vals, UP_N);
    map = up_imap_new(0, NULL);

    for (i = 0; i < UP_N; i++) {
        CuAssertTrue(tc, (uint32_t)i == up_imap_size(map));
        CuAssertTrue(tc, (size_t)i == _up_live(map));
        CuAssertPtrEquals(tc, NULL, (void *)up_imap_get(map, &vals[i]));
        CuAssertPtrEquals(tc, NULL, (void *)up_imap_delete(map, &vals[i]));
        for (;;) {
            CuAssertPtrEquals(tc, NULL, (void *)up_imap_set(map, &vals[i]));
            if (!up_imap_oom(map)) {
                break;
            }
        }
        for (j = 0; j < i; j++) {
            v = up_imap_get(map, &vals[j]);
            CuAssertPtrNotNull(tc, (void *)v);
            CuAssertIntEquals(tc, vals[j], *v);
        }
        for (;;) {
            v = up_imap_set(map, &vals[i]);
            if (NULL == v) {
                CuAssertTrue(tc, up_imap_oom(map));
                continue;
            }
            CuAssertTrue(tc, !up_imap_oom(map));
            CuAssertIntEquals(tc, vals[i], *v);
            break;
        }
        v = up_imap_get(map, &vals[i]);
        CuAssertPtrNotNull(tc, (void *)v);
        CuAssertIntEquals(tc, vals[i], *v);
        v = up_imap_delete(map, &vals[i]);
        CuAssertPtrNotNull(tc, (void *)v);
        CuAssertIntEquals(tc, vals[i], *v);
        CuAssertPtrEquals(tc, NULL, (void *)up_imap_get(map, &vals[i]));
        CuAssertPtrEquals(tc, NULL, (void *)up_imap_delete(map, &vals[i]));
        for (;;) {
            CuAssertPtrEquals(tc, NULL, (void *)up_imap_set(map, &vals[i]));
            if (!up_imap_oom(map)) {
                break;
            }
        }
        CuAssertTrue(tc, (uint32_t)(i + 1) == up_imap_size(map));
        CuAssertTrue(tc, (size_t)(i + 1) == _up_live(map));
    }

    MALLOC(marks, sizeof(int32_t) * UP_N);
    ZERO(marks, sizeof(int32_t) * UP_N);
    CuAssertTrue(tc, up_imap_scan(map, _up_mark, &marks));
    for (i = 0; i < UP_N; i++) {
        CuAssertIntEquals(tc, 1, marks[i]);
    }
    ZERO(marks, sizeof(int32_t) * UP_N);
    iter = 0;
    while (up_imap_iter(map, &iter, &item)) {
        _up_mark(item, &marks);// 恒返 true，套 CuAssert 只是个空断言；真判据是下面那圈 marks
    }
    for (i = 0; i < UP_N; i++) {
        CuAssertIntEquals(tc, 1, marks[i]);
    }
    FREE(marks);

    test_shuffle(&_up_rng, vals, UP_N);
    for (i = 0; i < UP_N; i++) {
        v = up_imap_delete(map, &vals[i]);
        CuAssertPtrNotNull(tc, (void *)v);
        CuAssertIntEquals(tc, vals[i], *v);
        CuAssertPtrEquals(tc, NULL, (void *)up_imap_get(map, &vals[i]));
        CuAssertTrue(tc, (uint32_t)(UP_N - i - 1) == up_imap_size(map));
        CuAssertTrue(tc, (size_t)(UP_N - i - 1) == _up_live(map));
        for (j = UP_N - 1; j > i; j--) {
            v = up_imap_get(map, &vals[j]);
            CuAssertPtrNotNull(tc, (void *)v);
            CuAssertIntEquals(tc, vals[j], *v);
        }
    }
    up_imap_free(map);
    FREE(vals);
}

// 往 map 里塞 UP_N 个 "s<i>" 字符串。_up_malloc 按 _up_fail_on 概率假失败,重试到成功为止
static void _up_fill_strings(CuTest *tc, up_smap *map) {
    char *str;
    int32_t i;
    for (i = 0; i < UP_N; i++) {
        str = NULL;
        while (NULL == str) {
            str = (char *)_up_malloc(16);
        }
        SNPRINTF(str, 16, "s%d", i);
        (void)up_smap_set(map, &str);
    }
    CuAssertTrue(tc, UP_N == (int32_t)up_smap_size(map));
}

// 上游 all() 的字符串段：elfree 必须在 clear 与 free 时对每个元素各调一次。
// 结束时分配计数归零即证明元素都还回了分配器（计数只记字符串元素，表本身走项目分配器）
static void test_hashmap_upstream_elfree(CuTest *tc) {
    up_smap *map;

    test_rng_init(&_up_rng, 1234567890123ULL);
    _up_allocs = 0;
    _up_fail_on = 1;
    map = up_smap_new(0, _up_free_str);

    _up_fill_strings(tc, map);

    up_smap_clear(map, 0);
    CuAssertTrue(tc, 0 == (int32_t)up_smap_size(map));

    _up_fill_strings(tc, map);

    up_smap_free(map);
    _up_fail_on = 0;
    CuAssertTrue(tc, 0 == _up_allocs);
}

/* _theap_insert 自己写全 hidx：拿一批填了垃圾的栈节点直接插，不预清零 */
static void test_heap_insert_no_prezero(CuTest *tc) {
    _theap h;
    _theap_init(&h, 0);
    int vals[] = { 30, 10, 50, 20, 40 };
    _hnode nodes[5];
    memset(nodes, 0xA5, sizeof(nodes));
    int i;
    for (i = 0; i < 5; i++) {
        nodes[i].val = vals[i];
        _theap_insert(&h, &nodes[i]);
    }
    CuAssertTrue(tc, 5 == _theap_size(&h));
    CuAssertTrue(tc, 10 == _theap_min(&h)->val);
    /* 逐个删根应取到升序 */
    int prev = -1;
    int top;
    for (i = 0; i < 5; i++) {
        top = _theap_min(&h)->val;
        CuAssertTrue(tc, top > prev);
        prev = top;
        _theap_remove(&h, _theap_min(&h));
    }
    CuAssertTrue(tc, 0 == _theap_size(&h));
    _theap_free(&h);
}

/* i32_que_del_at 改成了按段 memmove(最多两次 + 一次"绕回尾部"的单元素拷),回绕分支只有把
 * offset 推到各个位置才盖得全。用朴素模型穷举 offset × 元素数 × 删除位 对拍 */
static void test_queue_del_at(CuTest *tc) {
    i32_que q;
    int32_t model[16];
    int32_t v;
    uint32_t cap = 8, off, n, pos, i, m;
    i32_que_init(&q, cap);
    CuAssertTrue(tc, cap == i32_que_capacity(&q));
    for (off = 0; off < cap; off++) {
        for (n = 1; n <= cap; n++) {
            for (pos = 0; pos < n; pos++) {
                /* 把 offset 推到 off:先 push 再 pop 同样多个 */
                i32_que_clear(&q);
                v = 0;
                for (i = 0; i < off; i++) {
                    i32_que_push(&q, &v);
                }
                for (i = 0; i < off; i++) {
                    i32_que_pop(&q);
                }
                for (i = 0; i < n; i++) {
                    v = (int32_t)i;
                    i32_que_push(&q, &v);
                }
                CuAssertTrue(tc, cap == i32_que_capacity(&q));/* 全程不该扩容,否则 offset 被抹 */
                m = 0;
                for (i = 0; i < n; i++) {
                    if (i != pos) {
                        model[m++] = (int32_t)i;
                    }
                }
                i32_que_del_at(&q, pos);
                CuAssertTrue(tc, m == i32_que_size(&q));
                for (i = 0; i < m; i++) {
                    CuAssertTrue(tc, model[i] == *(int32_t *)i32_que_at(&q, i));
                }
            }
        }
    }
    /* 越界 pos 不动队列 */
    i32_que_clear(&q);
    v = 1;
    i32_que_push(&q, &v);
    i32_que_del_at(&q, 5);
    CuAssertTrue(tc, 1 == i32_que_size(&q));
    i32_que_free(&q);
}

/* ARR_DECL 的 _swap：大元素（200 字节）整元素交换，以及同位置交换是空操作 */
static void test_array_swap_large(CuTest *tc) {
    barr a;
    _big200 x, y;
    int32_t i;
    for (i = 0; i < 200; i++) {
        x.b[i] = (uint8_t)i;
        y.b[i] = (uint8_t)(255 - i);
    }
    barr_init(&a, 4);
    barr_push_back(&a, &x);
    barr_push_back(&a, &y);
    barr_swap(&a, 0, 1);
    CuAssertTrue(tc, 0 == memcmp(barr_at(&a, 0), &y, sizeof(y)));
    CuAssertTrue(tc, 0 == memcmp(barr_at(&a, 1), &x, sizeof(x)));
    barr_swap(&a, 0, 0);/* 同位置是空操作 */
    CuAssertTrue(tc, 0 == memcmp(barr_at(&a, 0), &y, sizeof(y)));
    barr_free(&a);
}

/* get_set:命中不覆写、未命中插入,两种情况都要返回"本元素"在表内的落点。
 * 落点得跟着 robin-hood 的搬动走,所以要灌到足够触发搬动与扩容的量 */
static void test_hashmap_get_set(CuTest *tc) {
    kv_map *map = kv_map_new(0, NULL);
    const _kv *p;
    _kv kv;
    int32_t found;
    int32_t i;
    CuAssertPtrNotNull(tc, map);

    SNPRINTF(kv.key, sizeof(kv.key), "k7");
    kv.val = 70;
    p = kv_map_get_set(map, &kv, &found);
    CuAssertPtrNotNull(tc, (void *)p);
    CuAssertTrue(tc, 0 == found);
    CuAssertTrue(tc, 70 == p->val);
    CuAssertTrue(tc, 1 == (int32_t)kv_map_size(map));

    /* 同 key 再来:命中已有,不覆写,count 不涨 */
    kv.val = 999;
    p = kv_map_get_set(map, &kv, &found);
    CuAssertPtrNotNull(tc, (void *)p);
    CuAssertTrue(tc, 1 == found);
    CuAssertTrue(tc, 70 == p->val);
    CuAssertTrue(tc, 1 == (int32_t)kv_map_size(map));

    /* found 传 NULL 合法 */
    SNPRINTF(kv.key, sizeof(kv.key), "k8");
    kv.val = 80;
    p = kv_map_get_set(map, &kv, NULL);
    CuAssertPtrNotNull(tc, (void *)p);
    CuAssertTrue(tc, 80 == p->val);

    /* 灌 2000 个:落点必须真的指向本 key 那一份 */
    for (i = 0; i < 2000; i++) {
        SNPRINTF(kv.key, sizeof(kv.key), "k%d", i);
        kv.val = i * 3;
        p = kv_map_get_set(map, &kv, &found);
        CuAssertPtrNotNull(tc, (void *)p);
        CuAssertTrue(tc, 0 == strcmp(p->key, kv.key));
        if (0 == found) {
            CuAssertTrue(tc, i * 3 == p->val);
        }
    }
    CuAssertTrue(tc, 2000 == (int32_t)kv_map_size(map));
    /* k7 / k8 是先插进去的,值必须还是旧的 */
    SNPRINTF(kv.key, sizeof(kv.key), "k7");
    CuAssertTrue(tc, 70 == (kv_map_get(map, &kv))->val);
    SNPRINTF(kv.key, sizeof(kv.key), "k8");
    CuAssertTrue(tc, 80 == (kv_map_get(map, &kv))->val);
    /* 其余全查得到 */
    for (i = 9; i < 2000; i++) {
        SNPRINTF(kv.key, sizeof(kv.key), "k%d", i);
        p = kv_map_get(map, &kv);
        CuAssertPtrNotNull(tc, (void *)p);
        CuAssertTrue(tc, i * 3 == p->val);
    }
    kv_map_free(map);
}

/* get / delete 加了 robin-hood 提前终止(dib 小于当前步数即判不在表里)。
 * 回移删除必须仍维持那条不变式,否则存在的 key 会被静默报成不存在 */
static void test_hashmap_miss_after_delete(CuTest *tc) {
    kv_map *map = kv_map_new(1024, NULL);
    const _kv *p;
    _kv kv;
    int32_t i;
    CuAssertPtrNotNull(tc, map);
    /* 600/1024 = 0.586,刚好压在 growat(0.6) 之下:表够密,探测链才够长 */
    for (i = 0; i < 600; i++) {
        SNPRINTF(kv.key, sizeof(kv.key), "key%d", i);
        kv.val = i;
        kv_map_set(map, &kv);
    }
    for (i = 0; i < 600; i += 2) {
        SNPRINTF(kv.key, sizeof(kv.key), "key%d", i);
        CuAssertPtrNotNull(tc, (void *)kv_map_delete(map, &kv));
    }
    for (i = 0; i < 600; i++) {
        SNPRINTF(kv.key, sizeof(kv.key), "key%d", i);
        p = kv_map_get(map, &kv);
        if (0 == i % 2) {
            CuAssertTrue(tc, NULL == p);
        } else {
            CuAssertPtrNotNull(tc, (void *)p);
            CuAssertTrue(tc, i == p->val);
        }
    }
    /* 从未存在过的 key */
    for (i = 600; i < 900; i++) {
        SNPRINTF(kv.key, sizeof(kv.key), "key%d", i);
        CuAssertTrue(tc, NULL == kv_map_get(map, &kv));
        CuAssertTrue(tc, NULL == kv_map_delete(map, &kv));
    }
    kv_map_free(map);
}

/* i32_que_pop_batch 按段 memcpy,回绕分支要靠把 offset 推到各个位置才盖得全。
 * 用朴素模型穷举 offset × 元素数 × 取多少 对拍 */
static void test_queue_pop_batch(CuTest *tc) {
    i32_que q;
    int32_t got[16];
    int32_t v;
    uint32_t cap = 8, off, n, want, i, exp;
    i32_que_init(&q, cap);
    for (off = 0; off < cap; off++) {
        for (n = 0; n <= cap; n++) {
            for (want = 0; want <= cap + 1; want++) {
                i32_que_clear(&q);
                v = 0;
                for (i = 0; i < off; i++) {
                    i32_que_push(&q, &v);
                }
                for (i = 0; i < off; i++) {
                    i32_que_pop(&q);
                }
                for (i = 0; i < n; i++) {
                    v = (int32_t)i;
                    i32_que_push(&q, &v);
                }
                CuAssertTrue(tc, cap == i32_que_capacity(&q));/* 全程不该扩容 */
                exp = (want < n) ? want : n;
                CuAssertTrue(tc, exp == i32_que_pop_batch(&q, got, want));
                for (i = 0; i < exp; i++) {
                    CuAssertTrue(tc, (int32_t)i == got[i]);/* FIFO 序 */
                }
                CuAssertTrue(tc, n - exp == i32_que_size(&q));
                /* 剩下的仍按 FIFO 接着出 */
                for (i = exp; i < n; i++) {
                    CuAssertTrue(tc, (int32_t)i == *(int32_t *)i32_que_pop(&q));
                }
            }
        }
    }
    /* 延迟分配态(maxsize 为 0)取不出东西,且不碰 ptr */
    i32_que lazy;
    i32_que_init(&lazy, 0);
    CuAssertTrue(tc, 0 == i32_que_pop_batch(&lazy, got, 4));
    i32_que_free(&lazy);
    i32_que_free(&q);
}

/* i32_mpq_empty / i32_fsqu_empty 与各自的 size 同口径 */
static void test_queue_empty_apis(CuTest *tc) {
    i32_mpq mq;
    i32_fsqu fq;
    int32_t v, out, i;
    i32_mpq_init(&mq, 4);
    CuAssertTrue(tc, i32_mpq_empty(&mq));
    v = 1;
    CuAssertTrue(tc, ERR_OK == i32_mpq_trypush(&mq, &v));
    CuAssertTrue(tc, !i32_mpq_empty(&mq));
    CuAssertTrue(tc, ERR_OK == i32_mpq_pop_sc(&mq, &out));
    CuAssertTrue(tc, i32_mpq_empty(&mq));
    i32_mpq_free(&mq);

    i32_fsqu_init(&fq, 2);
    CuAssertTrue(tc, i32_fsqu_empty(&fq));
    /* 灌到超出快路径容量,逼出溢出层:i32_fsqu_empty 必须把溢出层也算上 */
    for (i = 0; i < 16; i++) {
        i32_fsqu_push(&fq, &i);
    }
    CuAssertTrue(tc, !i32_fsqu_empty(&fq));
    CuAssertTrue(tc, 16 == (int32_t)i32_fsqu_size(&fq));
    for (i = 0; i < 16; i++) {
        CuAssertTrue(tc, ERR_OK == i32_fsqu_pop(&fq, &out));
        CuAssertTrue(tc, i == out);
    }
    CuAssertTrue(tc, i32_fsqu_empty(&fq));
    i32_fsqu_free(&fq);
}

/* capacity 为 1 按文档向上取整到 2，不再 abort（曾只在 mpq 后端的平台上崩） */
static void test_queue_capacity_one(CuTest *tc) {
    uptr_mpq mq;
    uptr_spsc sq;
    uptr_fsqu fq;
    uintptr_t v = 7, out = 0;
    uptr_mpq_init(&mq, 1);
    CuAssertTrue(tc, uptr_mpq_capacity(&mq) >= 2);
    CuAssertIntEquals(tc, ERR_OK, uptr_mpq_trypush(&mq, &v));
    CuAssertIntEquals(tc, ERR_OK, uptr_mpq_pop(&mq, &out));
    CuAssertTrue(tc, 7 == out);
    uptr_mpq_free(&mq);

    out = 0;
    uptr_spsc_init(&sq, 1);
    CuAssertTrue(tc, uptr_spsc_capacity(&sq) >= 2);
    CuAssertIntEquals(tc, ERR_OK, uptr_spsc_trypush(&sq, &v));
    CuAssertIntEquals(tc, ERR_OK, uptr_spsc_pop(&sq, &out));
    CuAssertTrue(tc, 7 == out);
    uptr_spsc_free(&sq);

    out = 0;
    uptr_fsqu_init(&fq, 1);
    CuAssertTrue(tc, uptr_fsqu_capacity(&fq) >= 2);
    uptr_fsqu_push(&fq, &v);
    CuAssertIntEquals(tc, ERR_OK, uptr_fsqu_pop(&fq, &out));
    CuAssertTrue(tc, 7 == out);
    uptr_fsqu_free(&fq);
}

/* i32_que_pop_back 后进先出，且要跨回绕：offset 推到环上各个位置，逐个弹尾的顺序与朴素模型一致 */
static void test_queue_pop_back(CuTest *tc) {
    i32_que q;
    i32_que lazy;
    int32_t v;
    int32_t *p;
    uint32_t cap = 4, off, n, i;
    i32_que_init(&q, cap);
    CuAssertTrue(tc, NULL == i32_que_pop_back(&q));
    for (off = 0; off < cap; off++) {
        for (n = 1; n <= cap; n++) {
            i32_que_clear(&q);
            v = 0;
            for (i = 0; i < off; i++) {
                i32_que_push(&q, &v);
            }
            for (i = 0; i < off; i++) {
                i32_que_pop(&q);
            }
            for (i = 0; i < n; i++) {
                v = (int32_t)i;
                i32_que_push(&q, &v);
            }
            CuAssertTrue(tc, cap == i32_que_capacity(&q));/* 全程不该扩容,否则 offset 被抹 */
            for (i = n; i > 0; i--) {
                p = i32_que_pop_back(&q);
                CuAssertTrue(tc, NULL != p && (int32_t)(i - 1) == *p);
            }
            CuAssertTrue(tc, NULL == i32_que_pop_back(&q));
        }
    }
    /* 与 pop 混用：头尾各取各的。offset 推到 3，0..3 落在下标 3,0,1,2 */
    i32_que_clear(&q);
    for (i = 0; i < 3; i++) {
        i32_que_push(&q, &v);
    }
    for (i = 0; i < 3; i++) {
        i32_que_pop(&q);
    }
    for (i = 0; i < 4; i++) {
        v = (int32_t)i;
        i32_que_push(&q, &v);
    }
    p = i32_que_pop(&q);
    CuAssertTrue(tc, NULL != p && 0 == *p);
    p = i32_que_pop_back(&q);
    CuAssertTrue(tc, NULL != p && 3 == *p);
    v = 9;
    i32_que_push(&q, &v);
    p = i32_que_pop_back(&q);
    CuAssertTrue(tc, NULL != p && 9 == *p);
    p = i32_que_pop(&q);
    CuAssertTrue(tc, NULL != p && 1 == *p);
    p = i32_que_pop_back(&q);
    CuAssertTrue(tc, NULL != p && 2 == *p);
    CuAssertTrue(tc, i32_que_empty(&q));
    i32_que_free(&q);
    /* 延迟分配态弹不出东西 */
    i32_que_init(&lazy, 0);
    CuAssertTrue(tc, NULL == i32_que_pop_back(&lazy));
    i32_que_free(&lazy);
}

/* 非 2 的幂的初始容量向上取到 2 的幂（下限 2），回绕全靠 mask：
   容量 5 取整成 8，写满后每轮出 3 进 3 让 offset 反复绕过环尾，顺序不乱、不扩容 */
static void test_queue_npow2_wrap(CuTest *tc) {
    u32_que q;
    uint32_t v, i, r;
    uint32_t next = 0, expect = 0;
    uint32_t *p;
    u32_que_init(&q, 5);
    CuAssertTrue(tc, 8 == u32_que_capacity(&q) && 7 == q.mask);
    for (i = 0; i < 8; i++) {
        v = next++;
        u32_que_push(&q, &v);
    }
    CuAssertTrue(tc, 8 == u32_que_capacity(&q) && u32_que_full(&q));
    for (r = 0; r < 20; r++) {
        for (i = 0; i < 3; i++) {
            p = u32_que_pop(&q);
            CuAssertTrue(tc, NULL != p && expect++ == *p);
        }
        for (i = 0; i < 3; i++) {
            v = next++;
            u32_que_push(&q, &v);
        }
        CuAssertTrue(tc, 8 == u32_que_size(&q) && 8 == u32_que_capacity(&q));
        for (i = 0; i < 8; i++) {
            p = u32_que_at(&q, i);
            CuAssertTrue(tc, NULL != p && expect + i == *p);
        }
    }
    for (i = 0; i < 8; i++) {
        p = u32_que_pop(&q);
        CuAssertTrue(tc, NULL != p && expect++ == *p);
    }
    CuAssertTrue(tc, u32_que_empty(&q) && next == expect);
    u32_que_free(&q);
    u32_que_init(&q, 3);
    CuAssertTrue(tc, 4 == u32_que_capacity(&q) && 3 == q.mask);
    u32_que_free(&q);
    u32_que_init(&q, 1);
    CuAssertTrue(tc, 2 == u32_que_capacity(&q) && 1 == q.mask);
    u32_que_free(&q);
}

/* =======================================================================
 * rbtree —— 侵入式红黑树
 * ======================================================================= */

/* 以 n 为根的子树：核对父指针回指、颜色位之外的低位为 0、红节点没有红孩子、左右黑高相等。
   返回黑高（空叶算 1），违反返回 -1；顺带数节点、记最大深度 */
static int32_t _rbt_bh(const rbt_node *n, const rbt_node *parent, uint32_t *cnt, int32_t depth, int32_t *maxdepth) {
    int32_t lh, rh, black;
    if (NULL == n) {
        return 1;
    }
    if (rbt_parent(n) != parent || 0 != (n->rbt_parent_color & 2)) {
        return -1;
    }
    black = (int32_t)(n->rbt_parent_color & 1);
    if (!black && ((NULL != n->rbt_left && 0 == (n->rbt_left->rbt_parent_color & 1))
        || (NULL != n->rbt_right && 0 == (n->rbt_right->rbt_parent_color & 1)))) {
        return -1;
    }
    (*cnt)++;
    if (depth > *maxdepth) {
        *maxdepth = depth;
    }
    lh = _rbt_bh(n->rbt_left, n, cnt, depth + 1, maxdepth);
    rh = _rbt_bh(n->rbt_right, n, cnt, depth + 1, maxdepth);
    if (lh < 0 || lh != rh) {
        return -1;
    }
    return lh + black;
}
/* 形状校验：根黑、红黑性质、节点数 == n、树高 h 满足 2^(h/2) <= n+1 */
static void _rbt_check_shape(CuTest *tc, const rbt_root *root, uint32_t n) {
    uint32_t cnt = 0;
    int32_t maxdepth = 0;
    CuAssertTrue(tc, (0 == n) == RBT_EMPTY_ROOT(root));
    if (NULL != root->rbt_node) {
        CuAssertTrue(tc, 1 == (root->rbt_node->rbt_parent_color & 1));
    }
    CuAssertTrue(tc, _rbt_bh(root->rbt_node, NULL, &cnt, 1, &maxdepth) > 0);
    CuAssertTrue(tc, n == cnt);
    CuAssertTrue(tc, ((uint64_t)1 << (maxdepth / 2)) <= (uint64_t)n + 1);
}
/* bykey 次序：形状 + 正向中序非降且相等段按插入序（底层根也能用） */
static void _rbk_check_root(CuTest *tc, const rbt_root *root, uint32_t n) {
    _rbe *e;
    _rbe *prev = NULL;
    uint32_t cnt = 0;
    _rbt_check_shape(tc, root, n);
    for (e = _rbk_entry(rbt_first(root)); NULL != e; e = _rbk_entry(rbt_next(&e->bykey))) {
        if (NULL != prev) {
            CuAssertTrue(tc, prev->key < e->key || (prev->key == e->key && prev->seq < e->seq));
        }
        prev = e;
        cnt++;
    }
    CuAssertTrue(tc, n == cnt);
}
/* bykey 树：次序 + 元素数 + 最左缓存 + 都标着在树里 + 反向遍历对称 */
static void _rbk_check(CuTest *tc, _rbk *t, uint32_t n) {
    _rbe *e;
    _rbe *prev = NULL;
    uint32_t cnt = 0;
    _rbk_check_root(tc, &t->root.rbt_root, n);
    CuAssertTrue(tc, n == _rbk_size(t) && (0 == n) == _rbk_empty(t));
    CuAssertTrue(tc, t->root.rbt_leftmost == rbt_first(&t->root.rbt_root));
    rbt_foreach(t, _rbk, e) {
        CuAssertTrue(tc, _rbk_linked(e));
        prev = e;
        cnt++;
    }
    CuAssertTrue(tc, n == cnt);
    CuAssertTrue(tc, prev == _rbk_last(t));
    for (e = _rbk_last(t); NULL != e; e = _rbk_prev(e)) {
        cnt--;
    }
    CuAssertTrue(tc, 0 == cnt);
}
/* byid 树：形状 + 元素数 + 中序严格递增 */
static void _rbi_check(CuTest *tc, _rbi *t, uint32_t n) {
    _rbe *e;
    _rbe *prev = NULL;
    uint32_t cnt = 0;
    _rbt_check_shape(tc, &t->root.rbt_root, n);
    CuAssertTrue(tc, n == _rbi_size(t));
    rbt_foreach(t, _rbi, e) {
        if (NULL != prev) {
            CuAssertTrue(tc, prev->id < e->id);
        }
        prev = e;
        cnt++;
    }
    CuAssertTrue(tc, n == cnt);
}

/* 空树 / 清零即不在树里 / 单元素 / 删掉带孩子的节点后节点清零、next/prev 不能顺着残留指针走 / 运行期重置 */
static void test_rbtree_empty(CuTest *tc) {
    rbt_root root = RBT_ROOT_INIT;
    rbt_root_cached rc = RBT_ROOT_CACHED_INIT;
    _rbk t;
    _rbe es[3];
    _rbe z;
    int32_t i;
    // 底层空树
    CuAssertTrue(tc, RBT_EMPTY_ROOT(&root));
    CuAssertTrue(tc, NULL == rbt_first(&root) && NULL == rbt_last(&root));
    CuAssertTrue(tc, NULL == rbt_first_postorder(&root) && NULL == rbt_next_postorder(NULL));
    CuAssertTrue(tc, NULL == rbt_first_cached(&rc));
    // 带类型的空树：查找、摘除、边界都安全返回
    _rbk_init(&t);
    CuAssertTrue(tc, NULL == _rbk_first(&t) && NULL == _rbk_last(&t) && NULL == _rbk_pop_first(&t));
    CuAssertTrue(tc, NULL == _rbk_find(&t, 1) && NULL == _rbk_extract(&t, 1));
    CuAssertTrue(tc, 0 == _rbk_count(&t, 1) && !_rbk_contains(&t, 1));
    CuAssertTrue(tc, NULL == _rbk_lower_bound(&t, 1) && NULL == _rbk_upper_bound(&t, 1));
    _rbk_check(tc, &t, 0);
    // 整体清零即不在树里；对它调 next / prev 返回 NULL
    memset(&z, 0, sizeof(z));
    CuAssertTrue(tc, !_rbk_linked(&z) && RBT_EMPTY_NODE(&z.bykey));
    CuAssertTrue(tc, NULL == rbt_next(&z.bykey) && NULL == rbt_prev(&z.bykey));
    // 单元素
    es[0].key = 5;
    es[0].seq = 0;
    CuAssertTrue(tc, NULL == _rbk_insert(&t, &es[0]));
    _rbk_check(tc, &t, 1);
    CuAssertTrue(tc, &es[0] == _rbk_first(&t) && &es[0] == _rbk_last(&t));
    CuAssertTrue(tc, NULL == _rbk_next(&es[0]) && NULL == _rbk_prev(&es[0]));
    CuAssertTrue(tc, &es[0] == _rbk_find(&t, 5) && _rbk_linked(&es[0]));
    _rbk_erase(&t, &es[0]);
    CuAssertTrue(tc, !_rbk_linked(&es[0]));
    _rbk_check(tc, &t, 0);
    // 1、2、3 升序插入后 2 是根且有两个孩子；删掉它，节点里的孩子指针仍是旧值，但节点已清零
    for (i = 0; i < 3; i++) {
        es[i].key = i + 1;
        es[i].seq = (uint32_t)i;
        _rbk_insert(&t, &es[i]);
    }
    CuAssertTrue(tc, &es[1].bykey == t.root.rbt_root.rbt_node);
    _rbk_erase(&t, &es[1]);
    CuAssertTrue(tc, RBT_EMPTY_NODE(&es[1].bykey));
    CuAssertTrue(tc, NULL == _rbk_next(&es[1]) && NULL == _rbk_prev(&es[1]));
    _rbk_check(tc, &t, 2);
    // 运行期重置（RBT_ROOT_INIT 只能用于定义）
    _rbk_init(&t);
    _rbk_check(tc, &t, 0);
    rbt_root_init(&root);
    CuAssertTrue(tc, RBT_EMPTY_ROOT(&root));
    rbt_root_cached_init(&rc);
    CuAssertTrue(tc, NULL == rbt_first_cached(&rc) && RBT_EMPTY_ROOT(&rc.rbt_root));
}

/* 升序 / 降序 / 锯齿 / 乱序各插 1000 个，每步校验；再乱序删光，每步校验，隔段核对剩余元素仍能按键找到本尊 */
static void test_rbtree_patterns(CuTest *tc) {
    const int32_t npat = 1000;
    _rbe *es;
    int32_t *order;
    _rbk t;
    test_rng rng;
    int32_t pat, i, j;
    MALLOC(es, sizeof(_rbe) * npat);
    MALLOC(order, sizeof(int32_t) * npat);
    test_rng_init(&rng, 20260923);
    for (pat = 0; pat < 4; pat++) {
        for (i = 0; i < npat; i++) {
            if (1 == pat) {
                order[i] = npat - 1 - i;
            } else if (2 == pat) {
                order[i] = (0 == (i & 1)) ? i / 2 : npat - 1 - i / 2;
            } else {
                order[i] = i;
            }
        }
        if (3 == pat) {
            test_shuffle(&rng, order, npat);
        }
        _rbk_init(&t);
        for (i = 0; i < npat; i++) {
            es[i].key = order[i];
            es[i].seq = (uint32_t)i;
            _rbk_insert(&t, &es[i]);
            _rbk_check(tc, &t, (uint32_t)i + 1);
        }
        /* order 改作删除用的下标序列 */
        for (i = 0; i < npat; i++) {
            order[i] = i;
        }
        test_shuffle(&rng, order, npat);
        for (i = 0; i < npat; i++) {
            _rbk_erase(&t, &es[order[i]]);
            CuAssertTrue(tc, NULL == _rbk_find(&t, es[order[i]].key));
            _rbk_check(tc, &t, (uint32_t)(npat - 1 - i));
            if (0 == i % 50) {
                for (j = i + 1; j < npat; j++) {
                    CuAssertTrue(tc, &es[order[j]] == _rbk_find(&t, es[order[j]].key));
                }
            }
        }
        CuAssertTrue(tc, _rbk_empty(&t));
    }
    FREE(order);
    FREE(es);
}

/* 重复键：同键按插入序排列；find 取相等段首；rbt_foreach_equal / count 覆盖整个相等段；extract 摘段首 */
static void test_rbtree_dup(CuTest *tc) {
    _rbe es[300];
    _rbk t;
    _rbe *it;
    uint32_t i, cnt, last;
    _rbk_init(&t);
    for (i = 0; i < 300; i++) {
        es[i].key = (int32_t)(i % 3) * 10;// 0、10、20 交替，各 100 个
        es[i].seq = i;
        CuAssertTrue(tc, NULL == _rbk_insert(&t, &es[i]));
    }
    _rbk_check(tc, &t, 300);
    CuAssertTrue(tc, &es[1] == _rbk_find(&t, 10));
    CuAssertTrue(tc, 100 == _rbk_count(&t, 10) && _rbk_contains(&t, 10));
    cnt = 0;
    last = 0;
    rbt_foreach_equal(&t, _rbk, 10, it) {
        CuAssertTrue(tc, 10 == it->key);
        CuAssertTrue(tc, 0 == cnt || it->seq > last);
        last = it->seq;
        cnt++;
    }
    CuAssertTrue(tc, 100 == cnt && 298 == last);
    /* next_equal 只看紧邻后继：段尾之后是 20 */
    CuAssertTrue(tc, NULL == _rbk_next_equal(&es[298], 10));
    CuAssertTrue(tc, &es[2] == _rbk_find(&t, 20));
    CuAssertTrue(tc, NULL == _rbk_find(&t, 5) && NULL == _rbk_find(&t, -1) && NULL == _rbk_find(&t, 30));
    CuAssertTrue(tc, 0 == _rbk_count(&t, 5) && !_rbk_contains(&t, 5));
    cnt = 0;
    rbt_foreach_equal(&t, _rbk, 5, it) {
        cnt++;
    }
    CuAssertTrue(tc, 0 == cnt);
    /* extract 摘段首并清零节点，find 顺延到同键的下一个 */
    CuAssertTrue(tc, &es[1] == _rbk_extract(&t, 10) && !_rbk_linked(&es[1]));
    CuAssertTrue(tc, &es[4] == _rbk_find(&t, 10) && 99 == _rbk_count(&t, 10));
    _rbk_check(tc, &t, 299);
}

/* 唯一键：冲突时 insert 返回已有元素、被拒元素字节不变；insert_check / insert_commit 两段式；
   MULTI 下 check 从不返回已有元素，插入点在相等段末尾，比最小还小时成为新最左 */
static void test_rbtree_unique(CuTest *tc) {
    _rbe es[64];
    _rbe clash, snap;
    _rbi t;
    _rbk tk;
    rbt_insert_pos pos = { 0 };
    uint32_t i;
    _rbi_init(&t);
    for (i = 0; i < 64; i++) {
        es[i].id = (i * 37 + 5) % 64;// 0..63 的一个排列，且不从最小键开始：非空树上会两次出现新最左
        es[i].key = (int32_t)es[i].id;
        es[i].seq = i;
        CuAssertTrue(tc, NULL == _rbi_insert(&t, &es[i]));
        CuAssertTrue(tc, t.root.rbt_leftmost == rbt_first(&t.root.rbt_root));
    }
    _rbi_check(tc, &t, 64);
    memset(&clash, 0xA5, sizeof(clash));
    clash.id = es[10].id;
    memcpy(&snap, &clash, sizeof(clash));
    CuAssertTrue(tc, &es[10] == _rbi_insert(&t, &clash));
    CuAssertTrue(tc, 0 == memcmp(&snap, &clash, sizeof(clash)));
    _rbi_check(tc, &t, 64);
    for (i = 0; i < 64; i++) {
        CuAssertTrue(tc, &es[i] == _rbi_find(&t, es[i].id));
    }
    CuAssertTrue(tc, NULL == _rbi_find(&t, 64));
    // 两段式：已有则 check 直接返回它；没有才构造元素再 commit
    CuAssertTrue(tc, &es[3] == _rbi_insert_check(&t, es[3].id, &pos));
    CuAssertTrue(tc, NULL == _rbi_insert_check(&t, 100, &pos));
    memset(&clash, 0, sizeof(clash));
    clash.id = 100;
    _rbi_insert_commit(&t, &clash, &pos);
    CuAssertTrue(tc, &clash == _rbi_find(&t, 100) && &clash == _rbi_last(&t));
    _rbi_check(tc, &t, 65);
    // 唯一键的 extract
    CuAssertTrue(tc, &clash == _rbi_extract(&t, 100) && NULL == _rbi_find(&t, 100));
    _rbi_check(tc, &t, 64);
    // MULTI 的两段式（元素的 bykey 与 byid 是两个节点，互不影响）
    _rbk_init(&tk);
    es[0].key = 10;
    es[0].seq = 0;
    _rbk_insert(&tk, &es[0]);
    es[1].key = 10;
    es[1].seq = 1;
    CuAssertTrue(tc, NULL == _rbk_insert_check(&tk, 10, &pos) && 0 == pos.leftmost);
    _rbk_insert_commit(&tk, &es[1], &pos);
    CuAssertTrue(tc, &es[1] == _rbk_next(&es[0]));
    es[2].key = 5;
    es[2].seq = 2;
    CuAssertTrue(tc, NULL == _rbk_insert_check(&tk, 5, &pos) && 1 == pos.leftmost);
    _rbk_insert_commit(&tk, &es[2], &pos);
    CuAssertTrue(tc, &es[2] == _rbk_first(&tk));
    _rbk_check(tc, &tk, 3);
    _rbi_check(tc, &t, 64);
}

/* lower_bound / upper_bound（同 std::set）：边界逐条核对，再与中序序列的线性扫描逐键比指针 */
static void test_rbtree_bound(CuTest *tc) {
    _rbe es[500];
    _rbk t;
    test_rng rng;
    _rbe *e;
    _rbe *lo;
    _rbe *up;
    int32_t k;
    uint32_t i, cnt;
    _rbk_init(&t);
    CuAssertTrue(tc, NULL == _rbk_lower_bound(&t, 0));
    CuAssertTrue(tc, NULL == _rbk_upper_bound(&t, 0));

    /* 偶数键 0..98，50 另有两个重复 */
    for (i = 0; i < 50; i++) {
        es[i].key = (int32_t)(i * 2);
        es[i].seq = i;
        _rbk_insert(&t, &es[i]);
    }
    for (i = 50; i < 52; i++) {
        es[i].key = 50;
        es[i].seq = i;
        _rbk_insert(&t, &es[i]);
    }
    CuAssertTrue(tc, &es[0] == _rbk_lower_bound(&t, -5));// 比最小还小：两者都是最小
    CuAssertTrue(tc, &es[0] == _rbk_upper_bound(&t, -5));
    CuAssertTrue(tc, &es[10] == _rbk_lower_bound(&t, 19));// 落在空隙：两者都是下一个
    CuAssertTrue(tc, &es[10] == _rbk_upper_bound(&t, 19));
    CuAssertTrue(tc, &es[10] == _rbk_lower_bound(&t, 20));// 恰好相等：lower 是它，upper 是下一个
    CuAssertTrue(tc, &es[11] == _rbk_upper_bound(&t, 20));
    CuAssertTrue(tc, &es[25] == _rbk_lower_bound(&t, 50));// 相等段：lower 是段首（最早插入），upper 越过整段
    CuAssertTrue(tc, &es[26] == _rbk_upper_bound(&t, 50));
    CuAssertTrue(tc, &es[49] == _rbk_lower_bound(&t, 98));// 等于最大：upper 为空
    CuAssertTrue(tc, NULL == _rbk_upper_bound(&t, 98));
    CuAssertTrue(tc, NULL == _rbk_lower_bound(&t, 99));// 比最大还大：两者都为空
    CuAssertTrue(tc, NULL == _rbk_upper_bound(&t, 99));
    /* 半开区间 [20, 40) 的遍历：lower_bound(20) 起，走到 lower_bound(40) 为止 */
    cnt = 0;
    up = _rbk_lower_bound(&t, 40);
    for (e = _rbk_lower_bound(&t, 20); e != up; e = _rbk_next(e)) {
        CuAssertTrue(tc, e->key >= 20 && e->key < 40);
        cnt++;
    }
    CuAssertTrue(tc, 10 == cnt);

    /* 随机键（键域小、重复多），每个查询键都与中序线性扫描的答案比指针 */
    _rbk_init(&t);
    test_rng_init(&rng, 17);
    for (i = 0; i < 500; i++) {
        es[i].key = (int32_t)(test_rng_next(&rng) % 100);
        es[i].seq = i;
        _rbk_insert(&t, &es[i]);
    }
    _rbk_check(tc, &t, 500);
    for (k = -2; k <= 101; k++) {
        lo = NULL;
        up = NULL;
        rbt_foreach(&t, _rbk, e) {
            if (NULL == lo && e->key >= k) {
                lo = e;
            }
            if (NULL == up && e->key > k) {
                up = e;
                break;
            }
        }
        CuAssertTrue(tc, lo == _rbk_lower_bound(&t, k));
        CuAssertTrue(tc, up == _rbk_upper_bound(&t, k));
        CuAssertTrue(tc, ((NULL != lo && k == lo->key) ? lo : NULL) == _rbk_find(&t, k));
    }
}

/* usage 收尾：整树销毁时的回调，数一下并释放元素 */
static void _rbu_free_cb(_rbu *e, void *ud) {
    (*(int32_t *)ud)++;
    FREE(e);
}
/* 正常用法示例（一个函数走完）：建 → 字符串键集合 → 组合键有序集合 → 定时器队列（底层接口）→ 整树销毁 */
static void test_rbtree_usage(CuTest *tc) {
    static const char *names[] = { "job.backup", "job.report", "job.clean", "user.alice",
                                   "user.bob", "job.index", "sys.gc", "user.carol" };
    static const int64_t scores[] = { 30, 90, 30, 70, 50, 90, 10, 50 };
    static const uint64_t dues[] = { 300, 100, 200, 100, 400, 100, 500, 250 };
    static const char *same_due[] = { "job.report", "user.alice", "job.index" };
    const uint32_t n = 8;
    _rbu_name byname;
    _rbu_score byscore;
    rbt_root_cached bydue = RBT_ROOT_CACHED_INIT;
    rbt_node **link;
    rbt_node *parent;
    rbt_node *cur;
    _rbu *t;
    _rbu *stop;
    _rbu twin;
    int32_t leftmost, cnt;
    int64_t sum;
    uint64_t now, last;
    uint32_t i;

    /* 1. 建：元素单独分配，同时挂上三棵树 */
    _rbu_name_init(&byname);
    _rbu_score_init(&byscore);
    for (i = 0; i < n; i++) {
        MALLOC(t, sizeof(_rbu));
        t->id = i;
        t->fired = 0;
        t->name = names[i];
        t->score = scores[i];
        t->due = dues[i];
        CuAssertTrue(tc, NULL == _rbu_name_insert(&byname, t));// 名字唯一
        CuAssertTrue(tc, NULL == _rbu_score_insert(&byscore, t));// (分数, id) 唯一
        /* 定时器用底层接口手写下降循环：相等往右，同一时刻按加入顺序排；一路往左才是新最左 */
        link = &bydue.rbt_root.rbt_node;
        parent = NULL;
        cur = *link;
        leftmost = 1;
        while (NULL != cur) {
            parent = cur;
            if (t->due < UPCAST(cur, _rbu, bydue)->due) {
                link = &cur->rbt_left;
            } else {
                link = &cur->rbt_right;
                leftmost = 0;
            }
            cur = *link;
        }
        rbt_link_node(&t->bydue, parent, link);
        rbt_insert_color_cached(&bydue, &t->bydue, leftmost);
    }

    /* 2. 字符串键集合：精确查找、重名被拒（返回已有的那个）、前缀区间按字典序遍历 */
    t = _rbu_name_find(&byname, "user.bob");
    CuAssertTrue(tc, NULL != t && 50 == t->score);
    CuAssertTrue(tc, NULL == _rbu_name_find(&byname, "user.dave"));
    memset(&twin, 0, sizeof(twin));
    twin.name = "job.clean";
    CuAssertTrue(tc, _rbu_name_find(&byname, "job.clean") == _rbu_name_insert(&byname, &twin));
    CuAssertTrue(tc, !_rbu_name_linked(&twin));
    cnt = 0;
    for (t = _rbu_name_lower_bound(&byname, "job."); NULL != t && 0 == strncmp(t->name, "job.", 4);
         t = _rbu_name_next(t)) {
        cnt++;
    }
    CuAssertIntEquals(tc, 4, cnt);// job.backup job.clean job.index job.report
    CuAssertStrEquals(tc, "job.backup", _rbu_name_lower_bound(&byname, "job.")->name);

    /* 3. 组合键有序集合：分数区间 [30, 70)、同分按 id、倒序取前三、改分数 = 先删再插 */
    cnt = 0;
    sum = 0;
    stop = _rbu_score_lower_bound(&byscore, (_rbu_sk){ 70, 0 });
    for (t = _rbu_score_lower_bound(&byscore, (_rbu_sk){ 30, 0 }); t != stop; t = _rbu_score_next(t)) {
        cnt++;
        sum += t->score;
    }
    CuAssertIntEquals(tc, 4, cnt);// backup(30) clean(30) bob(50) carol(50)
    CuAssertTrue(tc, 160 == sum);
    t = _rbu_score_lower_bound(&byscore, (_rbu_sk){ 90, 0 });
    CuAssertStrEquals(tc, "job.report", t->name);// 同分 90：id 小的在前
    t = _rbu_score_next(t);
    CuAssertStrEquals(tc, "job.index", t->name);
    CuAssertTrue(tc, NULL == _rbu_score_next(t));
    t = _rbu_score_last(&byscore);// 倒序：分数最高的三个
    CuAssertStrEquals(tc, "job.index", t->name);
    t = _rbu_score_prev(t);
    CuAssertStrEquals(tc, "job.report", t->name);
    t = _rbu_score_prev(t);
    CuAssertStrEquals(tc, "user.alice", t->name);
    t = _rbu_name_find(&byname, "sys.gc");// 改分数 10 → 95：参与比较的字段不能原地改，先删再插
    _rbu_score_erase(&byscore, t);
    t->score = 95;
    CuAssertTrue(tc, NULL == _rbu_score_insert(&byscore, t));
    CuAssertTrue(tc, t == _rbu_score_last(&byscore));
    CuAssertTrue(tc, t == _rbu_score_upper_bound(&byscore, (_rbu_sk){ 90, UINT32_MAX }));
    _rbt_check_shape(tc, &byscore.root.rbt_root, n);

    /* 4. 定时器队列（底层接口）：取消一个；同一时刻按加入顺序；推进时钟到 250，按到期先后逐个触发 */
    t = _rbu_name_find(&byname, "user.bob");
    rbt_erase_cached(&bydue, &t->bydue);
    RBT_CLEAR_NODE(&t->bydue);// 底层 erase 不清节点，要靠 RBT_EMPTY_NODE 判断就自己清
    CuAssertTrue(tc, RBT_EMPTY_NODE(&t->bydue));
    cnt = 0;
    for (cur = rbt_first_cached(&bydue); NULL != cur && 100 == UPCAST(cur, _rbu, bydue)->due; cur = rbt_next(cur)) {
        CuAssertStrEquals(tc, same_due[cnt], UPCAST(cur, _rbu, bydue)->name);
        cnt++;
    }
    CuAssertIntEquals(tc, 3, cnt);
    now = 250;
    last = 0;
    cnt = 0;
    while (NULL != (cur = rbt_first_cached(&bydue)) && (t = UPCAST(cur, _rbu, bydue))->due <= now) {
        rbt_erase_cached(&bydue, cur);
        RBT_CLEAR_NODE(cur);
        CuAssertTrue(tc, t->due >= last);
        last = t->due;
        t->fired = 1;
        cnt++;
    }
    CuAssertIntEquals(tc, 5, cnt);// 100×3、200、250；bob 已取消
    CuAssertStrEquals(tc, "job.backup", UPCAST(rbt_first_cached(&bydue), _rbu, bydue)->name);
    CuAssertTrue(tc, 1 == _rbu_name_find(&byname, "user.carol")->fired);
    CuAssertTrue(tc, 0 == _rbu_name_find(&byname, "user.bob")->fired);

    /* 5. 销毁：byname 挂着全部元素，clear 回调里释放；另两棵树的元素已随之释放，只重置根 */
    cnt = 0;
    _rbu_name_clear(&byname, _rbu_free_cb, &cnt);
    CuAssertIntEquals(tc, 8, cnt);
    CuAssertTrue(tc, _rbu_name_empty(&byname));
    _rbu_score_init(&byscore);
    rbt_root_cached_init(&bydue);
}

/* rbt_foreach_safe 里删当前元素：隔一个删一个，再删光 */
static void test_rbtree_iter_erase(CuTest *tc) {
    _rbe es[200];
    _rbk t;
    _rbe *e;
    _rbe *next;
    uint32_t i, k;
    _rbk_init(&t);
    for (i = 0; i < 200; i++) {
        es[i].key = (int32_t)i;
        es[i].seq = i;
        _rbk_insert(&t, &es[i]);
    }
    k = 0;
    rbt_foreach_safe(&t, _rbk, e, next) {
        if (0 == (k++ & 1)) {
            _rbk_erase(&t, e);
        }
    }
    _rbk_check(tc, &t, 100);
    rbt_foreach(&t, _rbk, e) {
        CuAssertTrue(tc, 1 == (e->key & 1));
    }
    rbt_foreach_safe(&t, _rbk, e, next) {
        _rbk_erase(&t, e);
    }
    _rbk_check(tc, &t, 0);
    for (i = 0; i < 200; i++) {
        CuAssertTrue(tc, !_rbk_linked(&es[i]));
    }
}

/* 同一元素挂两棵树：从一棵删掉不影响另一棵；节点先填 0xFF 再插（插入不要求预清零），删后直接重插 */
static void test_rbtree_two_trees(CuTest *tc) {
    _rbe es[128];
    _rbk bykey;
    _rbi byid;
    uint32_t i;
    _rbk_init(&bykey);
    _rbi_init(&byid);
    memset(es, 0xFF, sizeof(es));
    for (i = 0; i < 128; i++) {
        es[i].key = (int32_t)(i % 15);// 模数取奇数：同一个键下奇偶下标都有，删偶数再重插后新旧同段
        es[i].seq = i;
        es[i].id = 127 - i;
        _rbk_insert(&bykey, &es[i]);
        CuAssertTrue(tc, NULL == _rbi_insert(&byid, &es[i]));
    }
    _rbk_check(tc, &bykey, 128);
    _rbi_check(tc, &byid, 128);
    for (i = 0; i < 128; i += 2) {
        _rbk_erase(&bykey, &es[i]);
        CuAssertTrue(tc, !_rbk_linked(&es[i]) && _rbi_linked(&es[i]));
    }
    _rbk_check(tc, &bykey, 64);
    _rbi_check(tc, &byid, 128);
    for (i = 0; i < 128; i++) {
        CuAssertTrue(tc, &es[i] == _rbi_find(&byid, es[i].id));
    }
    /* 重插的 seq 比在树的都大，相等段仍按插入序 */
    for (i = 0; i < 128; i += 2) {
        es[i].seq = 1000 + i;
        _rbk_insert(&bykey, &es[i]);
    }
    _rbk_check(tc, &bykey, 128);
}

/* 底层接口：手写下降循环插到 cached 树，相等往右；返回是否成为新最左 */
static int32_t _rbt_ll_add(rbt_root_cached *rc, _rbe *e) {
    rbt_node **link = &rc->rbt_root.rbt_node;
    rbt_node *parent = NULL;
    rbt_node *cur = *link;
    int32_t leftmost = 1;
    while (NULL != cur) {
        parent = cur;
        if (e->key < _rbk_entry(cur)->key) {
            link = &cur->rbt_left;
        } else {
            link = &cur->rbt_right;
            leftmost = 0;
        }
        cur = *link;
    }
    rbt_link_node(&e->bykey, parent, link);
    rbt_insert_color_cached(rc, &e->bykey, leftmost);
    return leftmost;
}
/* 底层 cached 系列：最左始终等于 rbt_first、erase_cached 的两种返回值、replace_cached 换最左 */
static void test_rbtree_cached(CuTest *tc) {
    _rbe es[101];
    _rbe rep;
    _rbe rep2;
    int32_t keys[100];
    rbt_root_cached rc = RBT_ROOT_CACHED_INIT;
    test_rng rng;
    _rbe *ret;
    _rbe *mid = NULL;
    rbt_node *lm;
    int32_t minkey = INT32_MAX;
    uint32_t i, n = 0;
    for (i = 0; i < 100; i++) {
        keys[i] = (int32_t)i;
    }
    test_rng_init(&rng, 7);
    test_shuffle(&rng, keys, 100);
    for (i = 0; i < 100; i++) {
        es[i].key = keys[i];
        es[i].seq = i;
        if (50 == keys[i]) {
            mid = &es[i];
        }
        CuAssertTrue(tc, (keys[i] < minkey) == _rbt_ll_add(&rc, &es[i]));
        if (keys[i] < minkey) {
            minkey = keys[i];
        }
        CuAssertTrue(tc, rc.rbt_leftmost == rbt_first(&rc.rbt_root));
        n++;
    }
    /* 与最小键相等的排在它后面，不算新最左 */
    es[100].key = minkey;
    es[100].seq = 100;
    CuAssertTrue(tc, 0 == _rbt_ll_add(&rc, &es[100]));
    n++;
    CuAssertTrue(tc, rc.rbt_leftmost != &es[100].bykey);
    _rbk_check_root(tc, &rc.rbt_root, n);
    /* 删的不是最左：返回 NULL，缓存不变 */
    lm = rc.rbt_leftmost;
    CuAssertTrue(tc, NULL == rbt_erase_cached(&rc, &mid->bykey));
    n--;
    CuAssertTrue(tc, lm == rc.rbt_leftmost);
    /* 最左换成等键的新节点（新节点不需初始化），底层 replace 不清旧节点 */
    memset(&rep, 0xCC, sizeof(rep));
    ret = _rbk_entry(rc.rbt_leftmost);
    rep.key = ret->key;
    rep.seq = ret->seq;
    rbt_replace_node_cached(&rc, &ret->bykey, &rep.bykey);
    CuAssertTrue(tc, &rep.bykey == rc.rbt_leftmost);
    CuAssertTrue(tc, &rep.bykey == rbt_first(&rc.rbt_root));
    CuAssertTrue(tc, !RBT_EMPTY_NODE(&ret->bykey));
    _rbk_check_root(tc, &rc.rbt_root, n);
    /* 换掉非最左的最大元素（必是某个节点的右孩子）：最左缓存不变 */
    lm = rc.rbt_leftmost;
    ret = _rbk_entry(rbt_last(&rc.rbt_root));
    CuAssertTrue(tc, NULL != rbt_parent(&ret->bykey) && &ret->bykey == rbt_parent(&ret->bykey)->rbt_right);
    memset(&rep2, 0xCC, sizeof(rep2));
    rep2.key = ret->key;
    rep2.seq = ret->seq;
    rbt_replace_node_cached(&rc, &ret->bykey, &rep2.bykey);
    CuAssertTrue(tc, lm == rc.rbt_leftmost);
    CuAssertTrue(tc, &rep2.bykey == rbt_last(&rc.rbt_root));
    _rbk_check_root(tc, &rc.rbt_root, n);
    /* 反复删最左：每次返回新的最左，删空时返回 NULL */
    while (NULL != rc.rbt_leftmost) {
        lm = rc.rbt_leftmost;
        ret = _rbk_entry(rbt_next(lm));
        CuAssertTrue(tc, (NULL == ret ? NULL : &ret->bykey) == rbt_erase_cached(&rc, lm));
        n--;
        CuAssertTrue(tc, rc.rbt_leftmost == rbt_first(&rc.rbt_root));
    }
    CuAssertTrue(tc, 0 == n);
    CuAssertTrue(tc, RBT_EMPTY_ROOT(&rc.rbt_root));
}

/* replace：换掉有两个孩子的根，新节点接管位置与颜色，按键找到的是新节点，旧节点清零且不在遍历里；换最左时缓存跟着换 */
static void test_rbtree_replace(CuTest *tc) {
    _rbe es[50];
    _rbe rep;
    _rbe rep2;
    _rbe *victim;
    _rbe *e;
    _rbk t;
    uint32_t i;
    _rbk_init(&t);
    for (i = 0; i < 50; i++) {
        es[i].key = (int32_t)i;
        es[i].seq = i;
        _rbk_insert(&t, &es[i]);
    }
    victim = _rbk_entry(t.root.rbt_root.rbt_node);
    CuAssertTrue(tc, NULL != victim->bykey.rbt_left && NULL != victim->bykey.rbt_right);
    memset(&rep, 0xCC, sizeof(rep));
    rep.key = victim->key;
    rep.seq = victim->seq;
    _rbk_replace(&t, victim, &rep);
    CuAssertTrue(tc, &rep.bykey == t.root.rbt_root.rbt_node);
    CuAssertTrue(tc, &rep == _rbk_find(&t, rep.key));
    CuAssertTrue(tc, !_rbk_linked(victim));
    _rbk_check(tc, &t, 50);
    rbt_foreach(&t, _rbk, e) {
        CuAssertTrue(tc, e != victim);
    }
    victim = _rbk_first(&t);
    memset(&rep2, 0xCC, sizeof(rep2));
    rep2.key = victim->key;
    rep2.seq = victim->seq;
    _rbk_replace(&t, victim, &rep2);
    CuAssertTrue(tc, &rep2 == _rbk_first(&t));
    _rbk_check(tc, &t, 50);
}

/* clear 回调：数一下并释放元素 */
static void _rbe_free_cb(_rbe *e, void *ud) {
    (*(uint32_t *)ud)++;
    FREE(e);
}
/* 底层后序遍历：每个节点恰好一次、孩子先于父；clear(NULL) 把节点全部清零；元素单独 MALLOC 时在 clear 回调里释放 */
static void test_rbtree_postorder(CuTest *tc) {
    _rbe es[255];
    uint8_t seen[255];
    int32_t keys[255];
    _rbk t;
    test_rng rng;
    rbt_node *pn;
    rbt_node *nn;
    _rbe *pos;
    _rbe *c;
    uint32_t i, cnt;
    for (i = 0; i < 255; i++) {
        keys[i] = (int32_t)i;
    }
    test_rng_init(&rng, 11);
    test_shuffle(&rng, keys, 255);
    _rbk_init(&t);
    for (i = 0; i < 255; i++) {
        es[i].key = keys[i];
        es[i].seq = i;
        _rbk_insert(&t, &es[i]);
    }
    memset(seen, 0, sizeof(seen));
    cnt = 0;
    for (pn = rbt_first_postorder(&t.root.rbt_root); NULL != pn; pn = nn) {
        nn = rbt_next_postorder(pn);
        pos = _rbk_entry(pn);
        CuAssertTrue(tc, 0 == seen[pos->key]);
        c = _rbk_entry(pos->bykey.rbt_left);
        CuAssertTrue(tc, NULL == c || 1 == seen[c->key]);
        c = _rbk_entry(pos->bykey.rbt_right);
        CuAssertTrue(tc, NULL == c || 1 == seen[c->key]);
        seen[pos->key] = 1;
        cnt++;
    }
    CuAssertTrue(tc, 255 == cnt);
    _rbk_clear(&t, NULL, NULL);
    _rbk_check(tc, &t, 0);
    for (i = 0; i < 255; i++) {
        CuAssertTrue(tc, !_rbk_linked(&es[i]));
    }

    for (i = 0; i < 255; i++) {
        MALLOC(pos, sizeof(_rbe));
        pos->key = keys[i];
        pos->seq = i;
        _rbk_insert(&t, pos);
    }
    cnt = 0;
    _rbk_clear(&t, _rbe_free_cb, &cnt);
    CuAssertTrue(tc, 255 == cnt);
    _rbk_check(tc, &t, 0);
}

/* pop_first 按键从小到大、同键按插入序摘；extract 按键摘；摘下的节点都清零，可以直接重插 */
static void test_rbtree_pop_extract(CuTest *tc) {
    _rbe es[64];
    _rbk t;
    _rbe *e;
    int32_t prevkey = INT32_MIN;
    uint32_t prevseq = 0, i, n = 0;
    _rbk_init(&t);
    for (i = 0; i < 64; i++) {
        es[i].key = (int32_t)((i * 29) % 16);// 每个键 4 个；键 7 最早的两个是 es[3]、es[19]
        es[i].seq = i;
        _rbk_insert(&t, &es[i]);
    }
    CuAssertTrue(tc, &es[3] == _rbk_extract(&t, 7) && !_rbk_linked(&es[3]));
    CuAssertTrue(tc, &es[19] == _rbk_extract(&t, 7));
    CuAssertTrue(tc, 2 == _rbk_count(&t, 7));
    es[3].seq = 100;// 重插：seq 最大，排在同键末尾
    _rbk_insert(&t, &es[3]);
    _rbk_check(tc, &t, 63);
    while (NULL != (e = _rbk_pop_first(&t))) {
        CuAssertTrue(tc, !_rbk_linked(e));
        CuAssertTrue(tc, e->key > prevkey || (e->key == prevkey && e->seq > prevseq));
        prevkey = e->key;
        prevseq = e->seq;
        n++;
    }
    CuAssertTrue(tc, 63 == n);
    _rbk_check(tc, &t, 0);
}

/* churn 基准：按 (key, seq) 排序后与中序序列逐个比指针 */
static int _rbe_ref_cmp(const void *a, const void *b) {
    const _rbe *x = *(const _rbe *const *)a;
    const _rbe *y = *(const _rbe *const *)b;
    if (x->key != y->key) {
        return (x->key > y->key) - (x->key < y->key);
    }
    return (x->seq > y->seq) - (x->seq < y->seq);
}
static void _rbt_churn_cmp(CuTest *tc, _rbk *t, _rbe **live, uint32_t n) {
    _rbe *e;
    uint32_t i = 0;
    qsort(live, n, sizeof(_rbe *), _rbe_ref_cmp);
    rbt_foreach(t, _rbk, e) {
        CuAssertTrue(tc, i < n && live[i] == e);
        i++;
    }
    CuAssertTrue(tc, n == i);
}
/* 删除前的节点形状，对应摘除阶段的各分支：0 红叶 1 黑叶 2 只有右孩子 3 只有左孩子
   4 双孩子且后继是右孩子 5 双孩子且后继更深 */
static int32_t _rbt_shape_of(const rbt_node *n) {
    if (NULL == n->rbt_left && NULL == n->rbt_right) {
        return (0 == (n->rbt_parent_color & 1)) ? 0 : 1;
    }
    if (NULL == n->rbt_left) {
        return 2;
    }
    if (NULL == n->rbt_right) {
        return 3;
    }
    return (NULL == n->rbt_right->rbt_left) ? 4 : 5;
}
/* 随机插删 steps 步：不到 fill 或空了必插、满了必删，其余各半（各半是无漂移的随机游走，
   大树不先填满长不大）；键域只有容量的 1/4，逼出大量重复键。每 every 步全量校验 */
static void _rbt_churn(CuTest *tc, uint32_t cap, uint32_t fill, uint32_t steps, uint32_t every, uint64_t seed, uint32_t *shapes) {
    _rbe *pool;
    _rbe **live;
    _rbe **freel;
    _rbe *e;
    _rbk t;
    test_rng rng;
    uint32_t nlive = 0, nfree = cap, seq = 0, i, j, step;
    MALLOC(pool, sizeof(_rbe) * cap);
    MALLOC(live, sizeof(_rbe *) * cap);
    MALLOC(freel, sizeof(_rbe *) * cap);
    for (i = 0; i < cap; i++) {
        freel[i] = &pool[i];
    }
    _rbk_init(&t);
    test_rng_init(&rng, seed);
    for (step = 0; step < steps; step++) {
        if (nlive < fill || 0 == nlive || (0 != nfree && 0 == (test_rng_next(&rng) & 1))) {
            e = freel[--nfree];
            e->key = (int32_t)(test_rng_next(&rng) % (cap / 4 + 1));
            e->seq = seq++;
            _rbk_insert(&t, e);
            live[nlive++] = e;
        } else {
            j = (uint32_t)(test_rng_next(&rng) % nlive);
            e = live[j];
            shapes[_rbt_shape_of(&e->bykey)]++;
            _rbk_erase(&t, e);
            live[j] = live[--nlive];
            freel[nfree++] = e;
        }
        if (0 == step % every) {
            _rbk_check(tc, &t, nlive);
            _rbt_churn_cmp(tc, &t, live, nlive);
        }
    }
    _rbk_check(tc, &t, nlive);
    _rbt_churn_cmp(tc, &t, live, nlive);
    FREE(freel);
    FREE(live);
    FREE(pool);
}
/* 小树纯随机游走、每步校验；大树先填到 3/4 再插删、每 128 步校验；两轮合起来删除的六种形状都得走到 */
static void test_rbtree_churn(CuTest *tc) {
    uint32_t shapes[6] = { 0 };
    int32_t i;
    _rbt_churn(tc, 64, 0, 5000, 1, 1, shapes);
    _rbt_churn(tc, 4096, 3072, 50000, 128, 2, shapes);
    for (i = 0; i < 6; i++) {
        CuAssertTrue(tc, shapes[i] > 0);
    }
}

/* =======================================================================
 * rbtset —— rbtree 的持有型有序集合
 * ======================================================================= */

/* 中序：key 非降，相等段 val 递增（MULTI 下 val 即插入序；UNIQUE 下 key 严格递增）；反向遍历对称；
   first 与最左缓存一致；n 为期望元素数 */
static void _rsm_check(CuTest *tc, _rsm *s, uint32_t n) {
    _rse *e;
    _rse *prev = NULL;
    uint32_t cnt = 0;
    _rbt_check_shape(tc, &s->tree.root.rbt_root, n);
    CuAssertTrue(tc, n == _rsm_size(s));
    CuAssertTrue(tc, s->nfree <= s->maxfree);
    CuAssertTrue(tc, _rsm_first(s) == _rsm_item(_rsm_rb_entry(rbt_first(&s->tree.root.rbt_root))));
    for (e = _rsm_first(s); NULL != e; e = _rsm_next(e)) {
        if (NULL != prev) {
            CuAssertTrue(tc, prev->key < e->key || (prev->key == e->key && prev->val < e->val));
        }
        prev = e;
        cnt++;
    }
    CuAssertTrue(tc, n == cnt);
    CuAssertTrue(tc, prev == _rsm_last(s));
    for (e = _rsm_last(s); NULL != e; e = _rsm_prev(e)) {
        cnt--;
    }
    CuAssertTrue(tc, 0 == cnt);
}
static void test_rbtset_unique(CuTest *tc) {
    _rsu s;
    _rse item;
    _rse key;
    _rse *e;
    _rse *p;
    _rse *ret;
    _rse *next;
    int32_t found;
    int32_t i;
    _rsu_init(&s, NULL);
    // 空集合：查找、删除、边界、遍历都安全返回
    key.key = 1;
    CuAssertTrue(tc, 0 == _rsu_size(&s));
    CuAssertTrue(tc, NULL == _rsu_first(&s) && NULL == _rsu_last(&s));
    CuAssertTrue(tc, NULL == _rsu_find(&s, key.key) && 0 == _rsu_contains(&s, key.key) && 0 == _rsu_count(&s, key.key));
    CuAssertTrue(tc, NULL == _rsu_extract(&s, key.key));
    CuAssertTrue(tc, NULL == _rsu_lower_bound(&s, key.key) && NULL == _rsu_upper_bound(&s, key.key));
    // 乱序插入 0..99（37 与 100 互素，i*37%100 走遍全部）
    for (i = 0; i < 100; i++) {
        item.key = i * 37 % 100;
        item.val = item.key * 10;
        CuAssertTrue(tc, NULL == _rsu_insert_or_assign(&s, &item));
    }
    CuAssertTrue(tc, 100 == _rsu_size(&s));
    _rbt_check_shape(tc, &s.tree.root.rbt_root, 100);
    for (i = 0, e = _rsu_first(&s); NULL != e; i++, e = _rsu_next(e)) {
        CuAssertTrue(tc, i == e->key);
    }
    CuAssertTrue(tc, 100 == i);
    for (i = 99, e = _rsu_last(&s); NULL != e; i--, e = _rsu_prev(e)) {
        CuAssertTrue(tc, i == e->key);
    }
    // 覆盖：返回旧值副本，元素地址不变，个数不变
    key.key = 42;
    p = _rsu_find(&s, key.key);
    CuAssertTrue(tc, NULL != p && 420 == p->val && 1 == _rsu_count(&s, key.key));
    item.key = 42;
    item.val = -1;
    ret = _rsu_insert_or_assign(&s, &item);
    CuAssertTrue(tc, &s.spare == ret && 42 == ret->key && 420 == ret->val);
    CuAssertTrue(tc, p == _rsu_find(&s, key.key) && -1 == p->val && 100 == _rsu_size(&s));
    // 副本原样传回 insert_or_assign：节点拿回 420，副本变成 -1
    ret = _rsu_insert_or_assign(&s, &s.spare);
    CuAssertTrue(tc, &s.spare == ret && -1 == ret->val && 420 == p->val);
    // insert：已有不覆盖；没有才插入；found 可传 NULL
    item.val = 7;
    CuAssertTrue(tc, p == _rsu_insert(&s, &item, &found) && 1 == found && 420 == p->val);
    item.key = 1000;
    item.val = 5;
    e = _rsu_insert(&s, &item, &found);
    CuAssertTrue(tc, 0 == found && 1000 == e->key && 5 == e->val && 101 == _rsu_size(&s));
    key.key = 1000;
    CuAssertTrue(tc, e == _rsu_find(&s, key.key) && e == _rsu_last(&s));
    CuAssertTrue(tc, e == _rsu_insert(&s, &item, NULL));
    // extract：交出副本，找不到返回 NULL
    ret = _rsu_extract(&s, key.key);
    CuAssertTrue(tc, &s.spare == ret && 1000 == ret->key && 5 == ret->val && 100 == _rsu_size(&s));
    CuAssertTrue(tc, 0 == _rsu_contains(&s, key.key) && NULL == _rsu_extract(&s, key.key));
    // 副本当 key 用
    CuAssertTrue(tc, NULL == _rsu_find(&s, s.spare.key));
    // 遍历中删：先取 next 再 erase，删掉全部奇数键和 0（最左变动走缓存）
    for (e = _rsu_first(&s); NULL != e; e = next) {
        next = _rsu_next(e);
        if (0 != (e->key & 1) || 0 == e->key) {
            ret = _rsu_erase(&s, e);
            CuAssertTrue(tc, &s.spare == ret);
        }
    }
    CuAssertTrue(tc, 49 == _rsu_size(&s));
    _rbt_check_shape(tc, &s.tree.root.rbt_root, 49);
    CuAssertTrue(tc, 2 == _rsu_first(&s)->key && 98 == _rsu_last(&s)->key);
    CuAssertTrue(tc, _rsu_first(&s) == _rsu_item(_rsu_rb_entry(rbt_first(&s.tree.root.rbt_root))));
    // 边界
    key.key = 3;
    CuAssertTrue(tc, 4 == _rsu_lower_bound(&s, key.key)->key && 4 == _rsu_upper_bound(&s, key.key)->key);
    key.key = 4;
    CuAssertTrue(tc, 4 == _rsu_lower_bound(&s, key.key)->key && 6 == _rsu_upper_bound(&s, key.key)->key);
    key.key = -5;
    CuAssertTrue(tc, 2 == _rsu_lower_bound(&s, key.key)->key);
    key.key = 98;
    CuAssertTrue(tc, 98 == _rsu_lower_bound(&s, key.key)->key && NULL == _rsu_upper_bound(&s, key.key));
    key.key = 99;
    CuAssertTrue(tc, NULL == _rsu_lower_bound(&s, key.key));
    // free 后可直接再用
    _rsu_free(&s);
    CuAssertTrue(tc, 0 == _rsu_size(&s) && NULL == _rsu_first(&s) && RBT_EMPTY_ROOT(&s.tree.root.rbt_root));
    item.key = 1;
    CuAssertTrue(tc, NULL == _rsu_insert_or_assign(&s, &item) && 1 == _rsu_size(&s));
    _rsu_free(&s);
}
static void test_rbtset_multi(CuTest *tc) {
    _rsm s;
    _rse item;
    _rse key;
    _rse *e;
    _rse *nx;
    _rse *ret;
    int32_t found;
    int32_t i;
    int32_t n;
    _rsm_init(&s, NULL);
    // 0..5 各 10 个，val 为插入序
    for (i = 0; i < 60; i++) {
        item.key = i % 6;
        item.val = i;
        CuAssertTrue(tc, NULL == _rsm_insert_or_assign(&s, &item));
    }
    _rsm_check(tc, &s, 60);
    key.key = 3;
    CuAssertTrue(tc, 10 == _rsm_count(&s, key.key) && 1 == _rsm_contains(&s, key.key));
    CuAssertTrue(tc, 3 == _rsm_find(&s, key.key)->val);// 取最早插入的
    key.key = 9;
    CuAssertTrue(tc, 0 == _rsm_count(&s, key.key) && NULL == _rsm_find(&s, key.key));
    // 重复键总是插入，排到相等段末尾
    item.key = 3;
    item.val = 100;
    CuAssertTrue(tc, NULL == _rsm_insert_or_assign(&s, &item));
    key.key = 3;
    CuAssertTrue(tc, 11 == _rsm_count(&s, key.key));
    CuAssertTrue(tc, 100 == _rsm_prev(_rsm_upper_bound(&s, key.key))->val);
    _rsm_check(tc, &s, 61);
    // extract 删最早插入的那个
    ret = _rsm_extract(&s, key.key);
    CuAssertTrue(tc, &s.spare == ret && 3 == ret->key && 3 == ret->val);
    CuAssertTrue(tc, 9 == _rsm_find(&s, key.key)->val && 10 == _rsm_count(&s, key.key));
    // erase 删相等段中间指定的一个（key 2 的 val 20）
    key.key = 2;
    for (e = _rsm_find(&s, key.key); NULL != e && 20 != e->val; e = _rsm_next(e)) {
    }
    CuAssertTrue(tc, NULL != e && 2 == e->key);
    ret = _rsm_erase(&s, e);
    CuAssertTrue(tc, 20 == ret->val && 9 == _rsm_count(&s, key.key));
    for (i = 2, e = _rsm_find(&s, key.key); NULL != e && 2 == e->key; e = _rsm_next(e), i += 6) {
        if (20 == i) {
            i += 6;
        }
        CuAssertTrue(tc, i == e->val);
    }
    CuAssertTrue(tc, 62 == i);
    _rsm_check(tc, &s, 59);
    // MULTI 下 insert 同 std::multiset：已有相等的也照插，排到相等段末尾；再按指针删掉
    item.key = 4;
    item.val = 999;
    e = _rsm_insert(&s, &item, &found);
    CuAssertTrue(tc, 0 == found && 4 == e->key && 999 == e->val && 60 == _rsm_size(&s));
    key.key = 4;
    CuAssertTrue(tc, e == _rsm_prev(_rsm_upper_bound(&s, key.key)) && 11 == _rsm_count(&s, key.key));
    CuAssertTrue(tc, 999 == _rsm_erase(&s, e)->val && 59 == _rsm_size(&s));
    // 边界：lower 停在相等段首，upper 越过整段
    key.key = 4;
    CuAssertTrue(tc, 4 == _rsm_lower_bound(&s, key.key)->val && 5 == _rsm_upper_bound(&s, key.key)->val);
    // 按键删光一段
    key.key = 5;
    for (n = 0; NULL != _rsm_extract(&s, key.key); n++) {
    }
    CuAssertTrue(tc, 10 == n && 0 == _rsm_count(&s, key.key));
    key.key = 4;
    CuAssertTrue(tc, 4 == _rsm_last(&s)->key && NULL == _rsm_upper_bound(&s, key.key));
    _rsm_check(tc, &s, 49);
    // rbtree.h 的三个遍历宏同样适用于 rbtset 类型：全遍历、按键遍历（插入序）、遍历中删掉 key 3 的全部 10 个
    n = 0;
    rbt_foreach(&s, _rsm, e) {
        n++;
    }
    CuAssertTrue(tc, 49 == n);
    key.key = 3;
    n = 0;
    i = -1;
    rbt_foreach_equal(&s, _rsm, key.key, e) {
        CuAssertTrue(tc, 3 == e->key && e->val > i);
        i = e->val;
        n++;
    }
    CuAssertTrue(tc, 10 == n && (size_t)n == _rsm_count(&s, key.key));
    rbt_foreach_safe(&s, _rsm, e, nx) {
        if (3 == e->key) {
            _rsm_erase(&s, e);
        }
    }
    CuAssertTrue(tc, 0 == _rsm_count(&s, key.key));
    _rsm_check(tc, &s, 39);
    _rsm_free(&s);
    _rsm_check(tc, &s, 0);
}
static int32_t _rss_freed;
static void _rss_elfree(void *item) {
    _rss *e = (_rss *)item;
    FREE(e->name);
    _rss_freed++;
}
static void test_rbtset_elfree(CuTest *tc) {
    _rss_set s;
    _rss item;
    _rss *ret;
    int32_t i;
    _rss_freed = 0;
    _rss_set_init(&s, _rss_elfree);
    for (i = 0; i < 10; i++) {
        item.key = i;
        MALLOC(item.name, _RSS_NAME_LEN);
        SNPRINTF(item.name, _RSS_NAME_LEN, "n%d", i);
        CuAssertTrue(tc, NULL == _rss_set_insert_or_assign(&s, &item));
    }
    // 覆盖交出的旧值归调用方释放，不走 elfree
    item.key = 3;
    MALLOC(item.name, _RSS_NAME_LEN);
    SNPRINTF(item.name, _RSS_NAME_LEN, "new3");
    ret = _rss_set_insert_or_assign(&s, &item);
    CuAssertStrEquals(tc, "n3", ret->name);
    FREE(ret->name);
    CuAssertStrEquals(tc, "new3", _rss_set_find(&s, item.key)->name);
    // 删除交出的副本同样归调用方
    item.key = 4;
    ret = _rss_set_extract(&s, item.key);
    CuAssertStrEquals(tc, "n4", ret->name);
    CuAssertTrue(tc, 0 == _rss_freed);
    _rss_elfree(ret);
    CuAssertTrue(tc, 1 == _rss_freed);
    // free 对剩下的 9 个逐个调 elfree
    _rss_set_free(&s);
    CuAssertTrue(tc, 10 == _rss_freed && 0 == _rss_set_size(&s));
}
/* 随机增删与参照计数比对：MULTI 下 extract 删最早的、erase 删相等段最后一个，每 64 步全量校验；
   maxfree 为池上限，0 即不开池 */
static void _rsm_churn(CuTest *tc, size_t maxfree) {
    enum { NKEY = 64, STEPS = 20000 };
    _rsm s;
    _rse item;
    _rse key;
    _rse *e;
    _rse *ret;
    test_rng rng;
    uint32_t cnt[NKEY];
    uint32_t total = 0;
    int32_t seq = 0;
    int32_t want;
    int32_t step;
    uint64_t r;
    ZERO(cnt, sizeof(cnt));
    test_rng_init(&rng, 20260925ULL);
    _rsm_init(&s, NULL);
    _rsm_set_pool(&s, maxfree);
    for (step = 1; step <= STEPS; step++) {
        r = test_rng_next(&rng);
        key.key = (int32_t)(r % NKEY);
        switch ((r >> 32) % 4) {
        case 0:
        case 1:
            item.key = key.key;
            item.val = seq++;
            CuAssertTrue(tc, NULL == _rsm_insert_or_assign(&s, &item));
            cnt[key.key]++;
            total++;
            break;
        case 2:
            e = _rsm_find(&s, key.key);
            want = (NULL == e) ? -1 : e->val;
            ret = _rsm_extract(&s, key.key);
            CuAssertTrue(tc, (NULL == ret) == (0 == cnt[key.key]));
            if (NULL != ret) {
                CuAssertTrue(tc, want == ret->val);
                cnt[key.key]--;
                total--;
            }
            break;
        default:
            e = _rsm_upper_bound(&s, key.key);
            e = (NULL == e) ? _rsm_last(&s) : _rsm_prev(e);
            if (NULL != e && key.key == e->key) {
                want = e->val;
                CuAssertTrue(tc, want == _rsm_erase(&s, e)->val);
                cnt[key.key]--;
                total--;
            } else {
                CuAssertTrue(tc, 0 == cnt[key.key]);
            }
            break;
        }
        if (0 == step % 64) {
            _rsm_check(tc, &s, total);
            for (key.key = 0; key.key < NKEY; key.key++) {
                CuAssertTrue(tc, cnt[key.key] == _rsm_count(&s, key.key));
            }
        }
    }
    _rsm_free(&s);
    CuAssertTrue(tc, 0 == s.nfree && maxfree == s.maxfree);
}
static void test_rbtset_churn(CuTest *tc) {
    _rsm_churn(tc, 0);
    _rsm_churn(tc, 16);
}
/* 节点池：默认不留；开池后删除入池、插入先取刚放回的（地址相同即复用）；上限生效；调低上限与 free 当场释放。
   池里的节点有没有真的释放，由收尾的 not free 检查兜底 */
static void test_rbtset_pool(CuTest *tc) {
    _rsu s;
    _rse item;
    _rse *e[10];
    int32_t i;
    // 默认关：删除直接释放
    _rsu_init(&s, NULL);
    CuAssertTrue(tc, 0 == s.maxfree && 0 == s.nfree);
    item.key = 1;
    item.val = 1;
    _rsu_insert_or_assign(&s, &item);
    _rsu_extract(&s, item.key);
    CuAssertTrue(tc, 0 == s.nfree && NULL == s.freelist);
    // 开池，上限 4：删 6 个，前 4 个入池，后 2 个释放
    _rsu_set_pool(&s, 4);
    for (i = 0; i < 10; i++) {
        item.key = i;
        item.val = i;
        _rsu_insert_or_assign(&s, &item);
    }
    for (i = 0; i < 10; i++) {
        item.key = i;
        e[i] = _rsu_find(&s, item.key);
    }
    for (i = 0; i < 6; i++) {
        _rsu_erase(&s, e[i]);
        CuAssertTrue(tc, s.nfree == (size_t)(i < 4 ? i + 1 : 4));
    }
    // 插入先取最后放回的那个：依次拿到 e[3]、e[2]
    item.key = 100;
    item.val = 100;
    _rsu_insert_or_assign(&s, &item);
    CuAssertTrue(tc, e[3] == _rsu_find(&s, item.key) && 100 == e[3]->val && 3 == s.nfree);
    item.key = 101;
    _rsu_insert_or_assign(&s, &item);
    CuAssertTrue(tc, e[2] == _rsu_find(&s, item.key) && 2 == s.nfree);
    _rbt_check_shape(tc, &s.tree.root.rbt_root, 6);
    // 调低上限：多出来的当场释放
    _rsu_set_pool(&s, 1);
    CuAssertTrue(tc, 1 == s.nfree && 1 == s.maxfree);
    // free 连池一起释放，上限保留；之后再用照常
    _rsu_free(&s);
    CuAssertTrue(tc, 0 == s.nfree && NULL == s.freelist && 1 == s.maxfree && 0 == _rsu_size(&s));
    _rsu_insert_or_assign(&s, &item);
    _rsu_extract(&s, item.key);
    CuAssertTrue(tc, 1 == s.nfree);
    _rsu_free(&s);
    CuAssertTrue(tc, 0 == s.nfree);
}

void test_containers(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_mpq_basic);
    SUITE_ADD_TEST(suite, test_mpq_basic_sc);
    SUITE_ADD_TEST(suite, test_mpq_boundary);
    SUITE_ADD_TEST(suite, test_mpq_concurrent_mc);
    SUITE_ADD_TEST(suite, test_mpq_concurrent_sc);
    SUITE_ADD_TEST(suite, test_mpq_size_never_underreports);
    SUITE_ADD_TEST(suite, test_spsc_basic);
    SUITE_ADD_TEST(suite, test_spsc_boundary);
    SUITE_ADD_TEST(suite, test_spsc_concurrent);
    SUITE_ADD_TEST(suite, test_fsqu_basic);
    SUITE_ADD_TEST(suite, test_fsqu_trypush_full);
    SUITE_ADD_TEST(suite, test_fsqu_batch);
    SUITE_ADD_TEST(suite, test_fsqu_pop_sc);
    SUITE_ADD_TEST(suite, test_fsqu_default_cap);
    SUITE_ADD_TEST(suite, test_fsqu_overflow_fifo);
    SUITE_ADD_TEST(suite, test_fsqu_overflow_pop_batch);
    SUITE_ADD_TEST(suite, test_fsqu_overflow_push_batch);
    SUITE_ADD_TEST(suite, test_fsqu_trypush_no_overflow);
    SUITE_ADD_TEST(suite, test_fsqu_trypush_sticky);
    SUITE_ADD_TEST(suite, test_fsqu_never_overflow_free);
    SUITE_ADD_TEST(suite, test_fsqu_ring_backends);
    SUITE_ADD_TEST(suite, test_mpq_full_fails_fast);
    SUITE_ADD_TEST(suite, test_mpq_backoff_cap);
    SUITE_ADD_TEST(suite, test_mpq_pop_empty_vs_inflight);
    SUITE_ADD_TEST(suite, test_mpq_multiprod_conserve);
    SUITE_ADD_TEST(suite, test_bbq_basic);
    SUITE_ADD_TEST(suite, test_bbq_basic_sc);
    SUITE_ADD_TEST(suite, test_bbq_boundary);
    SUITE_ADD_TEST(suite, test_bbq_pop_empty_vs_inflight);
    SUITE_ADD_TEST(suite, test_bbq_epoch_wrap);
    SUITE_ADD_TEST(suite, test_bbq_multiprod_conserve);
    SUITE_ADD_TEST(suite, test_bbq_multicons_conserve);
    SUITE_ADD_TEST(suite, test_bbq_size_never_underreports);
    SUITE_ADD_TEST(suite, test_bbq_size_half_advance);
    SUITE_ADD_TEST(suite, test_mpq_pop_sc_batch);
    SUITE_ADD_TEST(suite, test_bbq_pop_sc_batch);
    SUITE_ADD_TEST(suite, test_mpq_batch_conserve);
    SUITE_ADD_TEST(suite, test_fsqu_ovf_drain_after_mpq_empty);
    SUITE_ADD_TEST(suite, test_fsqu_order_single_producer);
    SUITE_ADD_TEST(suite, test_fsqu_ring_order);
    SUITE_ADD_TEST(suite, test_chan_buffered_race);
    SUITE_ADD_TEST(suite, test_hashmap);
    SUITE_ADD_TEST(suite, test_hashmap_scan_iter);
    SUITE_ADD_TEST(suite, test_hashmap_with_hash_variants);
    SUITE_ADD_TEST(suite, test_hashmap_clear_update_cap);
    SUITE_ADD_TEST(suite, test_hashmap_upstream_churn);
    SUITE_ADD_TEST(suite, test_hashmap_upstream_elfree);
    SUITE_ADD_TEST(suite, test_hashmap_clear_cap);
    SUITE_ADD_TEST(suite, test_hashmap_clear_allocs);
    SUITE_ADD_TEST(suite, test_hashmap_new_round_and_spare);
    SUITE_ADD_TEST(suite, test_hashmap_iter_unversioned);
    SUITE_ADD_TEST(suite, test_hashmap_load_factor_survives_resize);
    SUITE_ADD_TEST(suite, test_heap);
    SUITE_ADD_TEST(suite, test_heap_remove_root);
    SUITE_ADD_TEST(suite, test_heap_remove_sift_up);
    SUITE_ADD_TEST(suite, test_heap_insert_no_prezero);
    SUITE_ADD_TEST(suite, test_queue_capacity_one);
    SUITE_ADD_TEST(suite, test_queue_pop_back);
    SUITE_ADD_TEST(suite, test_queue_npow2_wrap);
    SUITE_ADD_TEST(suite, test_slist_basic);
    SUITE_ADD_TEST(suite, test_slist_insert);
    SUITE_ADD_TEST(suite, test_slist_remove);
    SUITE_ADD_TEST(suite, test_slist_splice_iter);
    SUITE_ADD_TEST(suite, test_rbtree_empty);
    SUITE_ADD_TEST(suite, test_rbtree_patterns);
    SUITE_ADD_TEST(suite, test_rbtree_dup);
    SUITE_ADD_TEST(suite, test_rbtree_unique);
    SUITE_ADD_TEST(suite, test_rbtree_bound);
    SUITE_ADD_TEST(suite, test_rbtree_usage);
    SUITE_ADD_TEST(suite, test_rbtree_iter_erase);
    SUITE_ADD_TEST(suite, test_rbtree_two_trees);
    SUITE_ADD_TEST(suite, test_rbtree_cached);
    SUITE_ADD_TEST(suite, test_rbtree_replace);
    SUITE_ADD_TEST(suite, test_rbtree_postorder);
    SUITE_ADD_TEST(suite, test_rbtree_pop_extract);
    SUITE_ADD_TEST(suite, test_rbtree_churn);
    SUITE_ADD_TEST(suite, test_rbtset_unique);
    SUITE_ADD_TEST(suite, test_rbtset_multi);
    SUITE_ADD_TEST(suite, test_rbtset_elfree);
    SUITE_ADD_TEST(suite, test_rbtset_churn);
    SUITE_ADD_TEST(suite, test_rbtset_pool);
    SUITE_ADD_TEST(suite, test_queue);
    SUITE_ADD_TEST(suite, test_queue_lazy_trypush);
    SUITE_ADD_TEST(suite, test_queue_array_free_resets);
    SUITE_ADD_TEST(suite, test_queue_del_at);
    SUITE_ADD_TEST(suite, test_queue_pop_batch);
    SUITE_ADD_TEST(suite, test_queue_empty_apis);
    SUITE_ADD_TEST(suite, test_array_swap_large);
    SUITE_ADD_TEST(suite, test_hashmap_get_set);
    SUITE_ADD_TEST(suite, test_hashmap_miss_after_delete);
    SUITE_ADD_TEST(suite, test_array);
    SUITE_ADD_TEST(suite, test_array_negpos);
    SUITE_ADD_TEST(suite, test_array_ptr);
}

