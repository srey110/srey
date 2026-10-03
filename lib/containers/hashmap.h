#ifndef HASHMAP_H_
#define HASHMAP_H_

#include "base/base.h"

// 类型化 hashmap(robin-hood 开放寻址)。元素按值存在一个桶数组里,桶数恒为 2 的幂。
//
// 典型用法:
//   typedef struct { char *name; uint64_t handle; } ent;
//   #define ENT_HASH(e)     hash_str((e)->name)
//   #define ENT_CMP(a, b)   strcmp((a)->name, (b)->name)
//   HASHMAP_DECL(ent_map, ent, ENT_HASH, ENT_CMP)
//   ent_map *m = ent_map_new(1024, NULL);
//   ent e = { "k", 1 };
//   ent_map_set(m, &e);
//   ent *hit = ent_map_get(m, &e);                    // 查找时只需填好参与 HASH/CMP 的字段
//
// HASHMAP_DECL 生成(name 为生成的类型名):
//   name##_new(cap, elfree) / name##_free(m):cap 向上取 2 的幂、至少 16;elfree 可为 NULL
//   name##_clear(m, update_cap):清空并逐个调 elfree。update_cap 非 0 时把当前桶数记作新的 cap、不分配;
//       为 0 时若扩过容就把桶数组缩回 cap、重新分配一次(已是 cap 大小则不分配) —— 与名字给人的直觉相反
//   name##_set(m, item):插入;键已存在则原位覆盖并返回旧元素的副本,否则返回 NULL
//   name##_get_set(m, item, found):已有就返回表内那个,没有才插入并返回新插入的;found 非 NULL 时写入是否原已存在
//   name##_get(m, key) / name##_delete(m, key):查找 / 删除;delete 返回被删元素的副本;找不到返回 NULL
//   name##_*_with_hash:同上,哈希由调用方算好传入,必须与 HASHFN 的结果一致
//   name##_scan(m, iter, udata):逐个回调,iter 返回 0 即停;走完返回 1,被停下或回调里改了表返回 0
//   name##_iter(m, &i, &item):游标遍历,i 从 0 开始;取到返回 1,走完或表被增删过返回 0
//   name##_probe(m, pos):按桶下标直接取(pos 先与掩码),空桶返回 NULL
//   name##_size / name##_elsize / name##_oom:元素数 / sizeof(T) / 最近一次增删是否因容量溢出失败(只有插入会失败)
//   name##_set_load_factor(m, f) / name##_set_grow_by_power(m, p):负载因子夹到 [0.50, 0.95];扩容倍数 2^p,p 夹到 [1, 16]
//
// 契约:
//   1. set / delete 返回的副本放在表内的备用槽里,下一次 set 覆盖或 delete 前有效;它们都不调 elfree,
//      元素持有堆内存时调用方要自己对返回值调 elfree。只有 free / clear 会逐个调 elfree。
//   2. get / get_set / probe / iter 给的是桶内元素的指针,下一次插入或删除后就可能失效
//      (插入会挪动别的元素,删除会把后面的元素前移,两者都可能触发扩缩容);原位覆盖不挪位置。
//   3. scan / iter 期间增删会被检出并中止、记一条日志;32 位下游标放不下版本号,iter 期间增删是未定义行为。
//   4. 分配一律走项目的 MALLOC/CALLOC/FREE,_malloc 失败直接 exit,故 oom 与 new 返回 NULL 都只剩
//      "容量算出来溢出"这一种成因。set / get_set 插不进去时返回 NULL,要靠 oom 与"键不存在"区分。
//   5. HASHFN / CMPFN 是宏不是函数指针,形参直接就是 T const *(写法同 HEAP_DECL 的 LT),都不接受 NULL;
//      CMPFN 只看是否为 0。遍历回调收 T const *、返 int32_t。

#define HASHMAP_GROW_AT   0.60
#define HASHMAP_SHRINK_AT 0.10
#ifndef HASHMAP_LOAD_FACTOR
#define HASHMAP_LOAD_FACTOR HASHMAP_GROW_AT
#endif
// 游标里放不放版本号。默认按 size_t 宽度定,留 #ifndef 是为了在 64 位主机上
// 也能编出 32 位那套行为来测——否则 mk.sh m32 才走到的分支平时一次都跑不到
#ifndef HASHMAP_ITER_VERSIONED
    #if SIZE_MAX == UINT64_MAX
        #define HASHMAP_ITER_VERSIONED 1
    #else
        #define HASHMAP_ITER_VERSIONED 0
    #endif
#endif

// 入参写 T const * 而不是 const T *:T 是指针类型时,后者会被解析成指向 const 的指针。
// HASHMAP_DECL(name, T, HASHFN, CMPFN):name 生成的类型名,T 元素类型,
// HASHFN(e) 返回哈希,CMPFN(a, b) 三路比较返回 0 表示相等;两者的形参都是 T const *
#define HASHMAP_DECL(name, T, HASHFN, CMPFN)                                   \
typedef struct name##_bucket {                                                 \
    /* hash 与 dib 平铺放,别压成位域 */                                         \
    uint32_t hash;  /* 哈希的低 32 位,够用(桶数 < 2^32),扩容重排时不必再调 HASHFN */      \
    uint16_t dib;   /* 离理想桶的距离加一,0 表示空桶 */                             \
    T item;                                                                    \
} name##_bucket;                                                               \
typedef struct name {                                                          \
    void (*elfree)(void *item);   /* free / clear 时逐个调用,可为 NULL */       \
    size_t cap;                   /* 建表容量,删除缩容不会低于它 */             \
    size_t nbuckets;              /* 当前桶数,2 的幂 */                         \
    size_t count;                 /* 元素数 */                                  \
    size_t mask;                  /* nbuckets - 1,桶下标 = hash & mask */       \
    size_t growat;                /* 插入前 count 到此值就先扩容 */             \
    size_t shrinkat;              /* 删除后 count 不高于此值且桶数大于 cap 就减半 */ \
    uint8_t loadfactor;           /* 负载因子的百分数 */                        \
    uint8_t growpower;            /* 扩容倍数为 2^growpower */                  \
    uint8_t oom;                  /* 最近一次增删因容量溢出失败,每次增删先清零 */ \
    uint32_t version;             /* 结构变化(增删、扩缩容、clear)时加一,遍历靠它检出改动 */ \
    name##_bucket *buckets;                                                    \
    T spare;                      /* set 覆盖 / delete 返回的副本放这里 */      \
} name;                                                                        \
/* 按负载因子重算水位;缩容水位与扩容水位保持 SHRINK_AT : GROW_AT 的比例 */      \
static inline void name##_set_thresholds(name *m) {                            \
    m->growat = (m->nbuckets * m->loadfactor) / 100;                           \
    m->shrinkat = (m->growat * (size_t)(HASHMAP_SHRINK_AT * 100)) / (size_t)(HASHMAP_GROW_AT * 100); \
}                                                                              \
static inline void name##_set_grow_by_power(name *m, size_t power) {           \
    m->growpower = (uint8_t)(power < 1 ? 1 : power > 16 ? 16 : power);         \
}                                                                              \
static inline void name##_set_load_factor(name *m, double factor) {            \
    factor = _hashmap_clamp_lf(factor, m->loadfactor / 100.0);                 \
    m->loadfactor = (uint8_t)(factor * 100);                                   \
    name##_set_thresholds(m);                                                  \
}                                                                              \
static inline name *name##_new(size_t cap, void (*elfree)(void *item)) {       \
    size_t ncap = 16;                                                          \
    name *m;                                                                   \
    /* 容量向上取 2 的幂(下标才能用掩码代替取模),翻倍或乘桶大小会溢出就放弃 */  \
    while (ncap < cap) {                                                       \
        if (ncap > SIZE_MAX / 2) {                                             \
            return NULL;                                                       \
        }                                                                      \
        ncap *= 2;                                                             \
    }                                                                          \
    cap = ncap;                                                                \
    if (sizeof(name##_bucket) > SIZE_MAX / cap) {                              \
        return NULL;                                                           \
    }                                                                          \
    MALLOC(m, sizeof(name));                                                   \
    memset(m, 0, sizeof(name));                                                \
    m->elfree = elfree;                                                        \
    m->cap = cap;                                                              \
    m->nbuckets = cap;                                                         \
    m->mask = m->nbuckets - 1;                                                 \
    CALLOC(m->buckets, m->nbuckets, sizeof(name##_bucket));                    \
    m->growpower = 1;                                                          \
    m->loadfactor = (uint8_t)(_hashmap_clamp_lf(HASHMAP_LOAD_FACTOR, HASHMAP_GROW_AT) * 100); \
    name##_set_thresholds(m);                                                  \
    return m;                                                                  \
}                                                                              \
/* 对每个元素调 elfree,不动桶;数到 count 个就停,稀疏的大表不必扫完 */         \
static inline void name##_free_elements(name *m) {                             \
    size_t i, remain;                                                          \
    if (NULL == m->elfree) {                                                   \
        return;                                                                \
    }                                                                          \
    remain = m->count;                                                         \
    for (i = 0; 0 != remain && i < m->nbuckets; i++) {                         \
        if (m->buckets[i].dib) {                                               \
            m->elfree(&m->buckets[i].item);                                    \
            remain--;                                                          \
        }                                                                      \
    }                                                                          \
}                                                                              \
static inline void name##_free(name *m) {                                      \
    if (NULL == m) {                                                           \
        return;                                                                \
    }                                                                          \
    name##_free_elements(m);                                                   \
    FREE(m->buckets);                                                          \
    FREE(m);                                                                   \
}                                                                              \
static inline void name##_clear(name *m, int32_t update_cap) {                 \
    name##_bucket *newb;                                                       \
    name##_free_elements(m);                                                   \
    m->count = 0;                                                              \
    if (update_cap) {                                                          \
        m->cap = m->nbuckets;/* 保留已长到的桶数,之后缩容也不低于它 */             \
    } else if (m->nbuckets != m->cap) {/* 扩过容:换回建表大小的桶数组 */          \
        MALLOC(newb, sizeof(name##_bucket) * m->cap);                          \
        FREE(m->buckets);                                                      \
        m->buckets = newb;                                                     \
        m->nbuckets = m->cap;                                                  \
    }                                                                          \
    memset(m->buckets, 0, sizeof(name##_bucket) * m->nbuckets);                \
    m->mask = m->nbuckets - 1;                                                 \
    name##_set_thresholds(m);                                                  \
    m->version++;                                                              \
}                                                                              \
/* 与其余容器统一成 uint32_t。桶数到 2^32 时光桶数组就要 32G 以上,截断不可能发生 */ \
static inline uint32_t name##_size(const name *m) { return (uint32_t)m->count; } \
static inline uint32_t name##_elsize(const name *m) { (void)m; return (uint32_t)sizeof(T); } \
static inline int32_t name##_oom(const name *m) { return (int32_t)m->oom; }    \
/* 换成 new_cap(2 的幂)个桶并把元素重新落位;桶数组字节数会溢出返回 0,原表不动 */                            \
NOINLINE static UNUSED int32_t name##_resize(name *m, size_t new_cap) {        \
    name##_bucket *nb, *ob = m->buckets, *entry, *bucket, tmp;                 \
    size_t i, j, on = m->nbuckets, nmask = new_cap - 1;                        \
    if (sizeof(name##_bucket) > SIZE_MAX / new_cap) {                          \
        return 0;                                                              \
    }                                                                          \
    CALLOC(nb, new_cap, sizeof(name##_bucket));                                \
    for (i = 0; i < on; i++) {                                                 \
        entry = &ob[i];                                                        \
        if (!entry->dib) {                                                     \
            continue;                                                          \
        }                                                                      \
        entry->dib = 1;/* 按新掩码从理想桶重新探测;旧表随即整个丢弃,原地改写无妨 */                      \
        j = entry->hash & nmask;                                               \
        for (;;) {                                                             \
            bucket = &nb[j];                                                   \
            if (0 == bucket->dib) {                                            \
                *bucket = *entry;                                              \
                break;                                                         \
            }                                                                  \
            /* 同插入:离理想桶更远的抢位,被挤出来的接着往后找 */                                     \
            if (bucket->dib < entry->dib) {                                    \
                tmp = *bucket;                                                 \
                *bucket = *entry;                                              \
                *entry = tmp;                                                  \
            }                                                                  \
            j = (j + 1) & nmask;                                               \
            entry->dib += 1;                                                   \
        }                                                                      \
    }                                                                          \
    FREE(m->buckets);                                                          \
    m->buckets = nb;                                                           \
    m->nbuckets = new_cap;                                                     \
    m->mask = nmask;                                                           \
    name##_set_thresholds(m);                                                  \
    m->version++;                                                              \
    return 1;                                                                  \
}                                                                              \
/* 插入主体。getorset 为 0 即 set(已有则覆盖),为 1 即 get_set(已有则原样返回) */ \
static inline T *name##_insert_impl(name *m, T const *item, uint64_t hash,     \
                                    int32_t getorset, int32_t *found) {        \
    name##_bucket carry;/* 换位后接住被挤出来的那个,cur 随即指向它 */                           \
    T const *cur = item;/* 待落位的元素;首次换位前指向调用方的 item */                         \
    T *slot = NULL;                                                            \
    T titem;                                                                   \
    name##_bucket *bucket;                                                     \
    uint32_t chash;                                                            \
    uint16_t cdib;                                                             \
    size_t i, new_cap;                                                         \
    size_t imask;                                                              \
    name##_bucket *ib;                                                         \
    m->oom = 0;                                                                \
    if (NULL != found) {                                                       \
        *found = 0;                                                            \
    }                                                                          \
    /* 先扩容再落位,保证探测一定能碰到空桶;键已存在时也照扩,只是提前了一次 */    \
    if (m->count >= m->growat) {                                               \
        if (m->nbuckets > (SIZE_MAX >> m->growpower)) {                        \
            m->oom = 1;                                                        \
            return NULL;                                                       \
        }                                                                      \
        new_cap = m->nbuckets << m->growpower;                                 \
        if (new_cap <= m->nbuckets) {                                          \
            m->oom = 1;                                                        \
            return NULL;                                                       \
        }                                                                      \
        if (!name##_resize(m, new_cap)) {                                      \
            m->oom = 1;                                                        \
            return NULL;                                                       \
        }                                                                      \
    }                                                                          \
    chash = (uint32_t)_hashmap_clip(hash);                                     \
    cdib = 1;                                                                  \
    imask = m->mask;                                                           \
    ib = m->buckets;                                                           \
    i = chash & imask;                                                         \
    /* 从理想桶起线性探测,cdib 随步数加一 */                                       \
    for (;;) {                                                                 \
        bucket = &ib[i];                                                       \
        if (0 == bucket->dib) {/* 空桶:手上的元素落位 */                        \
            bucket->hash = chash;                                              \
            bucket->dib = cdib;                                                \
            bucket->item = *cur;                                               \
            m->count++;                                                        \
            m->version++;                                                      \
            if (!getorset) {                                                   \
                return NULL;                                                   \
            }                                                                  \
            return slot ? slot : &bucket->item;/* 中途换过位的话,新元素停在 slot */ \
        }                                                                      \
        /* 键已存在:只可能在第一次换位之前碰到,此时 cur 还是调用方的 item */          \
        if (chash == bucket->hash && 0 == CMPFN(cur, &bucket->item)) {         \
            if (NULL != found) {                                               \
                *found = 1;                                                    \
            }                                                                  \
            if (getorset) {                                                    \
                return &bucket->item;                                          \
            }                                                                  \
            m->spare = bucket->item;/* 原位覆盖,不挪位置,所以不涨 version */        \
            bucket->item = *cur;                                               \
            return &m->spare;                                                  \
        }                                                                      \
        /* robin-hood:手上的离理想桶比桶主更远就抢这个桶,被挤出来的接着往后找 */       \
        if (bucket->dib < cdib) {                                              \
            if (getorset && NULL == slot) {                                    \
                slot = &bucket->item;                                          \
            }                                                                  \
            /* 三方交换:先接住桶里的元素,再让本元素落位,最后才覆盖 carry                               \
               ——cur 这时可能正指着 carry.item,顺序颠倒会自己踩自己 */                          \
            titem = bucket->item;                                              \
            bucket->item = *cur;                                               \
            carry.hash = bucket->hash;                                         \
            carry.dib = bucket->dib;                                           \
            carry.item = titem;                                                \
            bucket->hash = chash;                                              \
            bucket->dib = cdib;                                                \
            chash = carry.hash;                                                \
            cdib = carry.dib;                                                  \
            cur = &carry.item;                                                 \
        }                                                                      \
        i = (i + 1) & imask;                                                   \
        cdib++;                                                                \
    }                                                                          \
}                                                                              \
static inline T *name##_set_with_hash(name *m, T const *item, uint64_t hash) { \
    return name##_insert_impl(m, item, hash, 0, NULL);                         \
}                                                                              \
static inline T *name##_set(name *m, T const *item) {                          \
    return name##_insert_impl(m, item, HASHFN(item), 0, NULL);                 \
}                                                                              \
static inline T *name##_get_set(name *m, T const *item, int32_t *found) {      \
    return name##_insert_impl(m, item, HASHFN(item), 1, found);                \
}                                                                              \
static inline T *name##_get_with_hash(const name *m, T const *key, uint64_t hash) { \
    name##_bucket *bucket;                                                     \
    uint64_t dib = 1;                                                          \
    size_t i;                                                                  \
    hash = _hashmap_clip(hash);                                                \
    i = (size_t)hash & m->mask;                                                \
    for (;;) {                                                                 \
        bucket = &m->buckets[i];                                               \
        /* 桶空,或桶主离理想桶比要找的还近:要找的键若在表里早该排在这之前,可以断定不存在 */ \
        if (bucket->dib < dib) {                                               \
            return NULL;                                                       \
        }                                                                      \
        if (bucket->hash == hash && 0 == CMPFN(key, &bucket->item)) { \
            return &bucket->item;                                              \
        }                                                                      \
        i = (i + 1) & m->mask;                                                 \
        dib++;                                                                 \
    }                                                                          \
}                                                                              \
static inline T *name##_get(const name *m, T const *key) {                     \
    return name##_get_with_hash(m, key, HASHFN(key));                          \
}                                                                              \
static inline T *name##_probe(name *m, uint64_t position) {                    \
    name##_bucket *bucket = &m->buckets[(size_t)position & m->mask];           \
    if (!bucket->dib) {                                                        \
        return NULL;                                                           \
    }                                                                          \
    return &bucket->item;                                                      \
}                                                                              \
static inline T *name##_delete_with_hash(name *m, T const *key, uint64_t hash) { \
    name##_bucket *bucket, *prev, *bks;                                        \
    size_t msk;                                                                \
    uint64_t dib = 1;                                                          \
    size_t i;                                                                  \
    hash = _hashmap_clip(hash);                                                \
    m->oom = 0;                                                                \
    i = (size_t)hash & m->mask;                                                \
    for (;;) {                                                                 \
        bucket = &m->buckets[i];                                               \
        if (bucket->dib < dib) {/* 同 get_with_hash:可以断定不存在 */            \
            return NULL;                                                       \
        }                                                                      \
        if (bucket->hash == hash && 0 == CMPFN(key, &bucket->item)) { \
            m->spare = bucket->item;                                           \
            bucket->dib = 0;                                                   \
            msk = m->mask;                                                     \
            bks = m->buckets;                                                  \
            /* 回移删除,不留墓碑:后面连续的元素逐个前移一格、dib 减一,        \
               碰到空桶或恰在理想桶(dib 为 1)的就停 */                          \
            for (;;) {                                                         \
                prev = bucket;                                                 \
                i = (i + 1) & msk;                                             \
                bucket = &bks[i];                                              \
                if (bucket->dib <= 1) {                                        \
                    prev->dib = 0;                                             \
                    break;                                                     \
                }                                                              \
                *prev = *bucket;                                               \
                prev->dib--;                                                   \
            }                                                                  \
            m->count--;                                                        \
            m->version++;                                                      \
            /* 删到缩容水位且桶数大于建表容量就减半;缩容失败不影响本次删除 */     \
            if (m->nbuckets > m->cap && m->count <= m->shrinkat) {             \
                name##_resize(m, m->nbuckets / 2);                             \
            }                                                                  \
            return &m->spare;                                                  \
        }                                                                      \
        i = (i + 1) & m->mask;                                                 \
        dib++;                                                                 \
    }                                                                          \
}                                                                              \
static inline T *name##_delete(name *m, T const *key) {                        \
    return name##_delete_with_hash(m, key, HASHFN(key));                       \
}                                                                              \
static inline int32_t name##_scan(name *m, int32_t (*iter)(T const *item, void *udata), void *udata) { \
    uint32_t ver = m->version;                                                 \
    size_t i, remain = m->count;                                               \
    int32_t go;                                                                \
    for (i = 0; 0 != remain && i < m->nbuckets; i++) {/* 数到 count 个就停 */  \
        if (m->buckets[i].dib) {                                               \
            go = iter(&m->buckets[i].item, udata);                             \
            if (ver != m->version) {/* 回调里增删过表:桶已挪动甚至换了数组,不能接着扫 */ \
                _hashmap_warn_modified(1);                                     \
                return 0;                                                      \
            }                                                                  \
            if (!go) {                                                         \
                return 0;                                                      \
            }                                                                  \
            remain--;                                                          \
        }                                                                      \
    }                                                                          \
    return 1;                                                                  \
}                                                                              \
static inline int32_t name##_iter(name *m, size_t *i, T **item) {              \
    name##_bucket *bucket;                                                     \
    size_t idx;                                                                \
    if (!_hashmap_iter_begin(i, m->version, &idx)) {/* 从游标取出下标并核对版本 */ \
        return 0;                                                              \
    }                                                                          \
    do {/* 跳过空桶 */                                                          \
        if (idx >= m->nbuckets) {                                              \
            _hashmap_iter_end(i, idx);                                         \
            return 0;                                                          \
        }                                                                      \
        bucket = &m->buckets[idx];                                             \
        idx++;                                                                 \
    } while (!bucket->dib);                                                    \
    _hashmap_iter_store(i, m->version, idx);/* 下一个下标连同版本号写回游标 */  \
    *item = &bucket->item;                                                     \
    return 1;                                                                  \
}

// 遍历期间表被增删时记一条 LOG_WARN。由 HASHMAP_DECL 生成的代码调用
static inline void _hashmap_warn_modified(int32_t is_scan) {
    if (0 != is_scan) {
        LOG_WARN("hashmap modified during scan, scan aborted.");
    } else {
        LOG_WARN("hashmap modified during iteration, iterator invalidated.");
    }
}
// 以下是 HASHMAP_DECL 展开后要用的辅助,与元素类型无关故不随实例复制。
// 宏惰性展开,定义排在宏之后不影响使用(展开点在调用方,那时它们都已可见)
// 负载因子夹到 [0.50, 0.95];NaN 回落到 deflt
static inline double _hashmap_clamp_lf(double factor, double deflt) {
    return factor != factor ? deflt :
           factor < 0.50 ? 0.50 :
           factor > 0.95 ? 0.95 : factor;
}
// 桶里的 hash 只有 32 位,存进去之前先裁掉高位
static inline uint64_t _hashmap_clip(uint64_t hash) {
    return hash & 0xFFFFFFFFull;
}
#if HASHMAP_ITER_VERSIONED
// 版本号塞在游标高 32 位,用来检出"遍历中增删"
static inline int32_t _hashmap_iter_begin(size_t *i, uint32_t ver, size_t *idx) {
    if (0 == *i) {
        *i = (size_t)ver << 32;
    } else if ((uint32_t)(*i >> 32) != ver) {
        _hashmap_warn_modified(0);
        return 0;
    }
    *idx = (uint32_t)*i;
    return 1;
}
static inline void _hashmap_iter_store(size_t *i, uint32_t ver, size_t idx) {
    *i = ((size_t)ver << 32) | idx;
}
// 版本号在,游标不会被误当成"还能接着走",扫完不必回写
static inline void _hashmap_iter_end(size_t *i, size_t idx) {
    (void)i;
    (void)idx;
}
#else
// 32 位下 size_t 没有高位放版本号,游标退化成裸下标:遍历中删元素是未定义行为
static inline int32_t _hashmap_iter_begin(size_t *i, uint32_t ver, size_t *idx) {
    (void)ver;
    *idx = *i;
    return 1;
}
static inline void _hashmap_iter_store(size_t *i, uint32_t ver, size_t idx) {
    (void)ver;
    *i = idx;
}
// 扫到头也要回写,游标须停在 nbuckets:否则调用方拿旧游标再调一次会从中途接着吐元素
static inline void _hashmap_iter_end(size_t *i, size_t idx) {
    *i = idx;
}
#endif

#endif//HASHMAP_H_
