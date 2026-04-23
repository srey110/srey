#include "test_containers.h"
#include "lib.h"

/* =======================================================================
 * mspc —— 无锁 MPMC 有界队列
 * ======================================================================= */

/* 单线程：基本入队出队、FIFO 顺序 */
static void test_mspc_basic(CuTest *tc) {
    mspc_ctx q;
    mspc_init(&q, 0);                   /* 0 → 默认容量 1024 */

    CuAssertTrue(tc, 1024 == q.capacity);
    CuAssertTrue(tc, 0 == mspc_size(&q));
    CuAssertTrue(tc, NULL == mspc_pop(&q));  /* 空队列出队返回 NULL */

    /* 入队 10 个元素（整数值转为指针，避免堆分配）*/
    for (uintptr_t i = 1; i <= 10; i++) {
        CuAssertTrue(tc, ERR_OK == mspc_push(&q, (void *)i));
    }
    CuAssertTrue(tc, 10 == mspc_size(&q));

    /* FIFO 顺序验证 */
    for (uintptr_t i = 1; i <= 10; i++) {
        CuAssertTrue(tc, (void *)i == mspc_pop(&q));
    }
    CuAssertTrue(tc, 0 == mspc_size(&q));
    CuAssertTrue(tc, NULL == mspc_pop(&q));
    mspc_free(&q);
}

/* 边界：队列填满后拒绝入队；非 2 的幂容量自动向上对齐 */
static void test_mspc_boundary(CuTest *tc) {
    mspc_ctx q;

    /* 容量 4，填满后 push 返回 ERR_FAILED */
    mspc_init(&q, 4);
    CuAssertTrue(tc, 4 == q.capacity);
    for (uintptr_t i = 1; i <= 4; i++) {
        CuAssertTrue(tc, ERR_OK == mspc_push(&q, (void *)i));
    }
    CuAssertTrue(tc, ERR_FAILED == mspc_push(&q, (void *)5));

    /* 消费 2 个后可再入队 2 个 */
    CuAssertTrue(tc, (void *)1 == mspc_pop(&q));
    CuAssertTrue(tc, (void *)2 == mspc_pop(&q));
    CuAssertTrue(tc, ERR_OK == mspc_push(&q, (void *)5));
    CuAssertTrue(tc, ERR_OK == mspc_push(&q, (void *)6));
    CuAssertTrue(tc, ERR_FAILED == mspc_push(&q, (void *)7));

    /* 出队顺序 */
    CuAssertTrue(tc, (void *)3 == mspc_pop(&q));
    CuAssertTrue(tc, (void *)4 == mspc_pop(&q));
    CuAssertTrue(tc, (void *)5 == mspc_pop(&q));
    CuAssertTrue(tc, (void *)6 == mspc_pop(&q));
    CuAssertTrue(tc, NULL == mspc_pop(&q));
    mspc_free(&q);

    /* 容量 5（非 2 的幂）→ 自动对齐为 8 */
    mspc_init(&q, 5);
    CuAssertTrue(tc, 8 == q.capacity);
    for (uintptr_t i = 1; i <= 8; i++) {
        CuAssertTrue(tc, ERR_OK == mspc_push(&q, (void *)i));
    }
    CuAssertTrue(tc, ERR_FAILED == mspc_push(&q, (void *)9));
    mspc_free(&q);
}

/* 并发：4 生产者 × 4 消费者，验证无丢失、无重复 */
#define _PROD_CNT    4
#define _CONS_CNT    4
#define _ITEMS_EACH  50000
#define _TOTAL       (_PROD_CNT * _ITEMS_EACH)

typedef struct { mspc_ctx *q; int id; } _prod_arg;

static void _producer(void *arg) {
    _prod_arg *a = (_prod_arg *)arg;
    uintptr_t start = (uintptr_t)a->id * _ITEMS_EACH + 1;
    uintptr_t end   = start + _ITEMS_EACH;
    for (uintptr_t v = start; v < end; v++) {
        while (ERR_OK != mspc_push(a->q, (void *)v)) {
            CPU_PAUSE();
        }
    }
}

static atomic_t   _consumed;
static atomic64_t _sum;

static void _consumer(void *arg) {
    mspc_ctx *q = (mspc_ctx *)arg;
    for (;;) {
        void *p = mspc_pop(q);
        if (NULL != p) {
            uint32_t prev = ATOMIC_ADD(&_consumed, 1);
            ATOMIC64_ADD(&_sum, (uintptr_t)p);
            if (prev + 1 >= (uint32_t)_TOTAL) {
                break;
            }
        } else {
            if (ATOMIC_GET(&_consumed) >= (uint32_t)_TOTAL) {
                break;
            }
            CPU_PAUSE();
        }
    }
}

static void test_mspc_concurrent(CuTest *tc) {
    mspc_ctx q;
    mspc_init(&q, 1024);

    _consumed = 0;
    _sum      = 0;

    /* 期望值之和：1+2+...+_TOTAL */
    int64_t expected = (int64_t)_TOTAL * (_TOTAL + 1) / 2;

    pthread_t  producers[_PROD_CNT];
    pthread_t  consumers[_CONS_CNT];
    _prod_arg  pargs[_PROD_CNT];

    /* 先启动消费者，避免生产者长时间自旋 */
    for (int i = 0; i < _CONS_CNT; i++) {
        consumers[i] = thread_creat(_consumer, &q);
    }
    for (int i = 0; i < _PROD_CNT; i++) {
        pargs[i].q  = &q;
        pargs[i].id = i;
        producers[i] = thread_creat(_producer, &pargs[i]);
    }

    for (int i = 0; i < _PROD_CNT; i++) thread_join(producers[i]);
    for (int i = 0; i < _CONS_CNT; i++) thread_join(consumers[i]);

    CuAssertTrue(tc, (uint32_t)_TOTAL == ATOMIC_GET(&_consumed));
    CuAssertTrue(tc, expected == (int64_t)ATOMIC64_GET(&_sum));
    mspc_free(&q);
}

/* =======================================================================
 * hashmap
 * ======================================================================= */

typedef struct { char key[32]; int val; } _kv;

static uint64_t _kv_hash(const void *item, uint64_t s0, uint64_t s1) {
    const _kv *kv = (const _kv *)item;
    return hashmap_sip(kv->key, strlen(kv->key), s0, s1);
}
static int _kv_cmp(const void *a, const void *b, void *udata) {
    (void)udata;
    return strcmp(((const _kv *)a)->key, ((const _kv *)b)->key);
}

static void test_hashmap(CuTest *tc) {
    struct hashmap *map = hashmap_new(sizeof(_kv), 0, 0, 0,
                                     _kv_hash, _kv_cmp, NULL, NULL);
    CuAssertPtrNotNull(tc, map);
    CuAssertTrue(tc, 0 == hashmap_count(map));

    /* 插入 100 条记录 */
    char key[32];
    for (int i = 0; i < 100; i++) {
        SNPRINTF(key, sizeof(key), "key_%d", i);
        _kv kv;
        SNPRINTF(kv.key, sizeof(kv.key), "key_%d", i);
        kv.val = i * 10;
        hashmap_set(map, &kv);
    }
    CuAssertTrue(tc, 100 == (int)hashmap_count(map));

    /* 查找验证 */
    for (int i = 0; i < 100; i++) {
        _kv lookup;
        SNPRINTF(lookup.key, sizeof(lookup.key), "key_%d", i);
        const _kv *found = (const _kv *)hashmap_get(map, &lookup);
        CuAssertPtrNotNull(tc, found);
        CuAssertTrue(tc, i * 10 == found->val);
    }

    /* 查找不存在的 key */
    _kv miss;
    SNPRINTF(miss.key, sizeof(miss.key), "not_exist");
    CuAssertTrue(tc, NULL == hashmap_get(map, &miss));

    /* 删除，数量减少 */
    _kv del;
    SNPRINTF(del.key, sizeof(del.key), "key_0");
    hashmap_delete(map, &del);
    CuAssertTrue(tc, 99 == (int)hashmap_count(map));
    CuAssertTrue(tc, NULL == hashmap_get(map, &del));

    /* 更新：同 key 再次 set 覆盖旧值 */
    _kv upd;
    SNPRINTF(upd.key, sizeof(upd.key), "key_1");
    upd.val = 9999;
    hashmap_set(map, &upd);
    const _kv *got = (const _kv *)hashmap_get(map, &upd);
    CuAssertTrue(tc, 9999 == got->val);
    CuAssertTrue(tc, 99 == (int)hashmap_count(map));

    hashmap_free(map);
}

/* =======================================================================
 * heap —— 最小堆（compare 返回非零表示 lhs 优先于 rhs）
 * ======================================================================= */

typedef struct { heap_node node; int val; } _hnode;

static int _heap_lt(const heap_node *a, const heap_node *b) {
    return UPCAST(a, _hnode, node)->val < UPCAST(b, _hnode, node)->val;
}

static void test_heap(CuTest *tc) {
    heap_ctx h;
    heap_init(&h, _heap_lt);
    CuAssertTrue(tc, 0 == h.nelts);

    /* 无序插入 5 个节点 */
    int vals[] = { 30, 10, 50, 20, 40 };
    _hnode nodes[5];
    for (int i = 0; i < 5; i++) {
        nodes[i].val = vals[i];
        heap_insert(&h, &nodes[i].node);
    }
    CuAssertTrue(tc, 5 == h.nelts);

    /* 堆顶始终是最小值 */
    CuAssertTrue(tc, 10 == UPCAST(h.root, _hnode, node)->val);

    /* 逐个出堆，顺序应为升序 */
    int expected[] = { 10, 20, 30, 40, 50 };
    for (int i = 0; i < 5; i++) {
        CuAssertTrue(tc, expected[i] == UPCAST(h.root, _hnode, node)->val);
        heap_dequeue(&h);
    }
    CuAssertTrue(tc, 0 == h.nelts);

    /* 插入后随机删除中间节点 */
    for (int i = 0; i < 5; i++) {
        nodes[i].val = vals[i];
        heap_insert(&h, &nodes[i].node);
    }
    /* 删除值为 20 的节点（nodes[3]）*/
    heap_remove(&h, &nodes[3].node);
    CuAssertTrue(tc, 4 == h.nelts);
    /* 堆顶仍是 10 */
    CuAssertTrue(tc, 10 == UPCAST(h.root, _hnode, node)->val);
}

/* =======================================================================
 * QUEUE_DECL —— 环形队列（自动扩容）
 * ======================================================================= */

QUEUE_DECL(int, _que);

static void test_queue(CuTest *tc) {
    _que_ctx q;
    _que_init(&q, 4);

    CuAssertTrue(tc, 0 == _que_size(&q));
    CuAssertTrue(tc, 4 == _que_maxsize(&q));
    CuAssertTrue(tc, _que_empty(&q));
    CuAssertTrue(tc, NULL == _que_pop(&q));
    CuAssertTrue(tc, NULL == _que_peek(&q));

    /* 入队 5 个，超出初始容量后自动扩容 */
    for (int i = 0; i < 5; i++) {
        _que_push(&q, &i);
    }
    CuAssertTrue(tc, 5 == (int)_que_size(&q));

    /* peek 不改变队列大小 */
    CuAssertTrue(tc, 0 == *_que_peek(&q));
    CuAssertTrue(tc, 5 == (int)_que_size(&q));

    /* FIFO 顺序出队 */
    for (int i = 0; i < 5; i++) {
        CuAssertTrue(tc, i == *_que_pop(&q));
    }
    CuAssertTrue(tc, _que_empty(&q));

    /* at() 访问 */
    for (int i = 0; i < 8; i++) {
        _que_push(&q, &i);
    }
    CuAssertTrue(tc, 0 == *_que_at(&q, 0));
    CuAssertTrue(tc, 7 == *_que_at(&q, 7));
    CuAssertTrue(tc, NULL == _que_at(&q, 8));

    /* clear 后队列为空 */
    _que_clear(&q);
    CuAssertTrue(tc, _que_empty(&q));

    _que_free(&q);
}

/* =======================================================================
 * ARRAY_DECL —— 动态数组
 * ======================================================================= */

ARRAY_DECL(int, _arr);

static void test_sarray(CuTest *tc) {
    _arr_ctx a;
    _arr_init(&a, 4);

    CuAssertTrue(tc, 0 == _arr_size(&a));
    CuAssertTrue(tc, 4 == _arr_maxsize(&a));
    CuAssertTrue(tc, _arr_empty(&a));
    CuAssertTrue(tc, NULL == _arr_front(&a));
    CuAssertTrue(tc, NULL == _arr_back(&a));

    /* push_back 超出容量后自动扩容 */
    for (int i = 1; i <= 8; i++) {
        _arr_push_back(&a, &i);
    }
    CuAssertTrue(tc, 8 == (int)_arr_size(&a));
    CuAssertTrue(tc, 1 == *_arr_front(&a));
    CuAssertTrue(tc, 8 == *_arr_back(&a));
    CuAssertTrue(tc, 3 == *_arr_at(&a, 2));

    /* 交换 */
    _arr_swap(&a, 0, 7);
    CuAssertTrue(tc, 8 == *_arr_front(&a));
    CuAssertTrue(tc, 1 == *_arr_back(&a));
    _arr_swap(&a, 0, 7);

    /* 在指定位置插入 */
    int v = 99;
    _arr_add(&a, &v, 2);
    CuAssertTrue(tc, 99 == *_arr_at(&a, 2));
    CuAssertTrue(tc, 9 == (int)_arr_size(&a));

    /* 删除（保持顺序）*/
    _arr_del(&a, 2);
    CuAssertTrue(tc, 3 == *_arr_at(&a, 2));
    CuAssertTrue(tc, 8 == (int)_arr_size(&a));

    /* pop_back */
    CuAssertTrue(tc, 8 == *_arr_pop_back(&a));
    CuAssertTrue(tc, 7 == (int)_arr_size(&a));

    /* del_nomove：用末尾元素填充被删位置 */
    _arr_del_nomove(&a, 0);
    CuAssertTrue(tc, 7 == *_arr_front(&a));   /* 末尾元素移到首位 */
    CuAssertTrue(tc, 6 == (int)_arr_size(&a));

    /* clear 不释放内存 */
    _arr_clear(&a);
    CuAssertTrue(tc, _arr_empty(&a));
    CuAssertTrue(tc, NULL == _arr_pop_back(&a));

    _arr_free(&a);
}

/* ======================================================================= */

void test_containers(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_mspc_basic);
    SUITE_ADD_TEST(suite, test_mspc_boundary);
    SUITE_ADD_TEST(suite, test_mspc_concurrent);
    SUITE_ADD_TEST(suite, test_hashmap);
    SUITE_ADD_TEST(suite, test_heap);
    SUITE_ADD_TEST(suite, test_queue);
    SUITE_ADD_TEST(suite, test_sarray);
}
