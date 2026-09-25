#ifndef BBQ_H_
#define BBQ_H_

#include "base/macro.h"

// 无锁多生产者有界队列(分块版)。接口签名与 MPQ_DECL 一致,差别在算法:
// mpq 是一条全局 enq 计数器上 CAS 抢位,撞输就重试;本实现把容量切成 nblk 个块、
// 每块 blksz 个槽,生产者在当前块上 FAA 抢位——抢到就不会失败,没有重试也就没有退避。
// 代价是每次入队两次 FAA(抢位 + 发布),以及下面三条行为差异。
//
// 与 mpq 的行为差异,调用方必须知道:
//   1. 空间按块归还。填满后要把整整一个块(blksz 条)消费完,trypush 才会重新成功。
//   2. 本块只要有一个生产者处在"抢到槽、还没写完"之间,消费者连该块里更早的、已经
//      写好的条目也取不到,pop 返回 1(在途)。mpq 只会被队头那一条挡住。
//      这道守卫要等本块已抢的槽全部提交才解除,与消费者落后多少无关;生产者不被抢占时
//      窗口很短,多见于"消费追平生产"时。
//   3. 容量上限 2^20(16 块 x BBQ_MAX_BLKSZ),取整后超过就在 init 时 ASSERTAB 中止;mpq 可到 2^31。
//
// 容量会被取整:先向上取到 2 的幂(最小 8),再按元素大小切块,capacity() 返回取整后的值。
// 块数按元素大小定:元素越大,同样的块头开销能摊的槽越少,块就切得越大(见 _bbq_nblk)。
//
// 消费者侧由调用方约定,同 MPQ_DECL:
//   name##_pop     多消费者安全
//   name##_pop_sc  单消费者优化(全路径无 CAS / FAA)
// 与 mpq 不同:pop_sc 不维护 reserved,同一队列只能固定用其中一种,同一线程里也不能交替混用

#define BBQ_DEFAULT_CAP 1024 // 默认容量
#define BBQ_MIN_CAP 8 // 最小容量:切成 4 块、每块 2 槽
#define BBQ_MAX_BLKSZ 65536 // 每块槽数上限,由游标里 off 字段的位宽定
// 慢路径重试上限。每次重试要么是自己推进了一个块、要么是看见别人推进了,都是真进展;
// 超限就当满处理,调用方降级到溢出层,不影响正确性
#define BBQ_RETRY_MAX 16
// 块游标打包成 (epoch << 20 | off):epoch 是该块本代对应的块序号,防 ABA;off 是块内计数。
// 两段都在一个 64 位里,一次原子读写就能拿到自洽的一对
#define BBQ_OFF_BITS  20
#define BBQ_OFF_MASK  (((uint64_t)1 << BBQ_OFF_BITS) - 1)
#define BBQ_OFF(c)    ((uint32_t)((uint64_t)(c) & BBQ_OFF_MASK))
#define BBQ_VSN(c)    ((uint64_t)(c) >> BBQ_OFF_BITS)
#define BBQ_CUR(v, o) ((((uint64_t)(v)) << BBQ_OFF_BITS) | (uint64_t)(o))

// 入参写 T const * 而不是 const T *：T 是指针类型(如 void *)时，后者会被解析成指向 const 的指针，调用方传普通指针就告警。
// BBQ_DECL(name, T)：name 生成的类型名，T 元素类型
#define BBQ_DECL(name, T)                                                       \
/* 块头三条 cache line。allocated 与 committed 必须分开:生产者猛抽 allocated 的同时  \
   消费者每次 pop 都要读 committed,不能合并成一条。消费者那对可以合住,           \
   生产者只在块边界读一次 consumed。不挂对齐属性——blocks 是 MALLOC 出来的,        \
   malloc 只保证 16 字节对齐,挂了也兑现不了;靠整行宽的步长保证不同组不落在同一行 */  \
typedef struct {                                                                \
    atomic64_t allocated; /* 已被抢占的槽数,可短暂超过 blksz(越界的抢占不占槽) */    \
    char _pad0[CACHELINE_SIZE - sizeof(atomic64_t)];                            \
    atomic64_t committed; /* 已写完数据的槽"数量"——是计数不是前缀,乱序提交要靠      \
                             allocated == committed 才能判定前缀就绪 */           \
    char _pad1[CACHELINE_SIZE - sizeof(atomic64_t)];                            \
    atomic64_t reserved;  /* 已被消费者预订的槽数;只有多消费者 pop 维护,生产者从不读 */\
    atomic64_t consumed;  /* 已拷贝完成的槽数;生产者靠它判断该块能否回收 */          \
    char _pad2[CACHELINE_SIZE - 2 * sizeof(atomic64_t)];                        \
} name##_block;                                                                 \
typedef struct {                                                                \
    name##_block *blocks;                                                       \
    T *cells;               /* nblk * blksz 个,块 i 的数据在 [i*blksz, ...) */    \
    uint32_t capacity;      /* nblk * blksz */                                  \
    uint32_t nblk;                                                              \
    uint32_t idxmask;       /* nblk - 1 */                                      \
    uint32_t blksz;                                                             \
    CACHELINE_ALIGN atomic64_t phead;/* 生产头,与 chead 各占一条 cache line */     \
    CACHELINE_ALIGN atomic64_t chead;/* 消费头 */                                \
} name;                                                                         \
static inline void name##_init(name *q, uint32_t capacity) {                    \
    uint32_t i;                                                                 \
    uint64_t cur;                                                               \
    ASSERTAB(NULL != q, ERRSTR_NULLP);                                          \
    capacity = (0 == capacity) ? BBQ_DEFAULT_CAP : capacity;                    \
    q->capacity = pow2_ceil(capacity < BBQ_MIN_CAP ? BBQ_MIN_CAP : capacity);   \
    q->nblk = _bbq_nblk(q->capacity, (uint32_t)sizeof(T));                      \
    q->idxmask = q->nblk - 1;                                                   \
    q->blksz = q->capacity / q->nblk;                                           \
    ASSERTAB(q->blksz <= BBQ_MAX_BLKSZ, "block size overflow.");                \
    ASSERTAB(sizeof(T) <= SIZE_MAX / (size_t)q->capacity, "byte size overflow.");\
    MALLOC(q->blocks, sizeof(name##_block) * (size_t)q->nblk);                  \
    MALLOC(q->cells, sizeof(T) * (size_t)q->capacity);                          \
    ATOMIC64_SET_RELAXED(&q->phead, (atomic64_t)q->nblk);                               \
    ATOMIC64_SET_RELAXED(&q->chead, (atomic64_t)q->nblk);                               \
    /* 0 号块是起点(本代全空),其余块装成"上一代已跑完",于是接管任何一块都是同一个    \
       判据,没有 0 号块特例。两个头从 nblk 而不是 0 起步也是为了这个 */             \
    for (i = 0; i < q->nblk; i++) {                                             \
        cur = (0 == i) ? BBQ_CUR(q->nblk, 0) : BBQ_CUR(i, q->blksz);            \
        ATOMIC64_SET_RELAXED(&q->blocks[i].allocated, (atomic64_t)cur);                 \
        ATOMIC64_SET_RELAXED(&q->blocks[i].committed, (atomic64_t)cur);                 \
        ATOMIC64_SET_RELAXED(&q->blocks[i].reserved, (atomic64_t)cur);                  \
        ATOMIC64_SET_RELAXED(&q->blocks[i].consumed, (atomic64_t)cur);                  \
    }                                                                           \
}                                                                               \
static inline void name##_free(name *q) {                                       \
    if (NULL == q) {                                                            \
        return;                                                                 \
    }                                                                           \
    FREE(q->blocks);                                                            \
    FREE(q->cells);                                                             \
    q->capacity = 0;                                                            \
    q->nblk = 0;                                                                \
    q->idxmask = 0;                                                             \
    q->blksz = 0;                                                               \
}                                                                               \
static inline uint32_t name##_capacity(const name *q) { return q->capacity; }   \
static inline uint32_t name##_elsize(const name *q) { (void)q; return (uint32_t)sizeof(T); }\
/* 保守快照:先读消费侧后读生产侧,只会把空报成非空,不会把非空报成空。                 \
   按 allocated 计数,已抢槽未发布的也算在内——fsqu 的漏唤醒守卫依赖这个方向。         \
   phead 可能比实际落后一块(推块三步没做完时下一块已能写入,消费头也可能先过去),     \
   所以生产侧的块序号取块游标里的 epoch 而不是 phead,该块满了还要看下一块是否已换代 */\
static inline uint32_t name##_size(name *q) {                                   \
    uint64_t chd = (uint64_t)ATOMIC64_GET(&q->chead);                           \
    uint64_t cons = (uint64_t)ATOMIC64_GET(&q->blocks[chd & q->idxmask].consumed);\
    uint64_t phd = (uint64_t)ATOMIC64_GET(&q->phead);                           \
    uint64_t alloc = (uint64_t)ATOMIC64_GET(&q->blocks[phd & q->idxmask].allocated);\
    uint64_t nalloc;                                                            \
    uint32_t oalloc, ocons;                                                     \
    int64_t n;                                                                  \
    if (BBQ_OFF(alloc) >= q->blksz) {                                           \
        nalloc = (uint64_t)ATOMIC64_GET(                                        \
            &q->blocks[(BBQ_VSN(alloc) + 1) & q->idxmask].allocated);           \
        if (BBQ_VSN(nalloc) == BBQ_VSN(alloc) + 1) {                            \
            alloc = nalloc;                                                     \
        }                                                                       \
    }                                                                           \
    oalloc = BBQ_OFF(alloc);                                                    \
    ocons = BBQ_OFF(cons);                                                      \
    oalloc = (oalloc > q->blksz) ? q->blksz : oalloc;                           \
    ocons = (ocons > q->blksz) ? q->blksz : ocons;                              \
    n = (int64_t)(BBQ_VSN(alloc) - chd) * (int64_t)q->blksz                     \
        + (int64_t)oalloc - (int64_t)ocons;                                     \
    if (n <= 0) {                                                               \
        return 0;                                                               \
    }                                                                           \
    return (n > (int64_t)q->capacity) ? q->capacity : (uint32_t)n;              \
}                                                                               \
/* 两头不等时两次原子读就够,直接报非空:生产头领先时中间必有整块没消费完,消费头     \
   暂时领先一块(推块途中)时报非空只是高估。相等才去读块游标。方向同 size */          \
static inline int32_t name##_empty(name *q) {                                   \
    uint64_t chd = (uint64_t)ATOMIC64_GET(&q->chead);                           \
    if ((uint64_t)ATOMIC64_GET(&q->phead) != chd) {                             \
        return 0;                                                               \
    }                                                                           \
    return 0 == name##_size(q);                                                 \
}                                                                               \
/* 入队:在当前块 FAA 抢一个槽,抢到就写数据再 FAA 发布,不会失败也不重试。            \
   队满立即 ERR_FAILED,不等待。抢槽那次 FAA 是全屏障,也是入队的定序点,             \
   调用方的丢唤醒握手靠它,不要降级。                                              \
   元素按指针传(同 MPQ_DECL) */                                                  \
static inline int32_t name##_trypush(name *q, T const *data) {                  \
    name##_block *blk;                                                          \
    name##_block *nblk;                                                         \
    uint64_t phd, cons, old;                                                    \
    uint32_t idx;                                                               \
    uint32_t retry = 0;                                                         \
    for (;;) {                                                                  \
        phd = (uint64_t)ATOMIC64_GET(&q->phead);                                \
        idx = (uint32_t)(phd & q->idxmask);                                     \
        blk = &q->blocks[idx];                                                  \
        /* 先看一眼再 FAA:满队列下不预检的话 allocated 会被抬到没边 */             \
        if (BBQ_OFF(ATOMIC64_GET(&blk->allocated)) < q->blksz) {                \
            old = (uint64_t)ATOMIC64_ADD(&blk->allocated, 1);                   \
            if (BBQ_OFF(old) < q->blksz) {                                      \
                memcpy(&q->cells[(size_t)idx * q->blksz + BBQ_OFF(old)],        \
                       data, sizeof(T));                                        \
                ATOMIC64_ADD(&blk->committed, 1);/* 顺序承重:必须在拷贝之后 */    \
                return ERR_OK;                                                  \
            }                                                                   \
        }                                                                       \
        nblk = &q->blocks[(phd + 1) & q->idxmask];                              \
        cons = (uint64_t)ATOMIC64_GET(&nblk->consumed);                         \
        if (BBQ_VSN(cons) < phd + 1 - q->nblk                                   \
            || (BBQ_VSN(cons) == phd + 1 - q->nblk                              \
                && BBQ_OFF(cons) != q->blksz)) {                                \
            /* 下一块的上一代还没消费干净就是满;phead 已被别人推过则是假满,重来 */  \
            if ((uint64_t)ATOMIC64_GET(&q->phead) == phd                        \
                || ++retry > BBQ_RETRY_MAX) {                                   \
                return ERR_FAILED;                                              \
            }                                                                   \
            continue;                                                           \
        }                                                                       \
        /* 三步顺序都承重:committed 先于 allocated,两者都先于 phead */            \
        _bbq_maxset(&nblk->committed, BBQ_CUR(phd + 1, 0));                     \
        _bbq_maxset(&nblk->allocated, BBQ_CUR(phd + 1, 0));                     \
        _bbq_maxset(&q->phead, phd + 1);                                        \
        if (++retry > BBQ_RETRY_MAX) {                                          \
            return ERR_FAILED;                                                  \
        }                                                                       \
    }                                                                           \
}                                                                               \
/* 出队(多消费者安全)。返回 1 表示"本块有生产者抢了槽还没写完",调用方据此区分真空与  \
   在途;注意这比 mpq 更容易返回 1,见文件头第 2 条行为差异 */                       \
static inline int32_t name##_pop(name *q, T *out) {                             \
    name##_block *blk;                                                          \
    name##_block *nblk;                                                         \
    uint64_t chd, resv, comm, alloc;                                            \
    uint32_t idx;                                                               \
    ASSERTAB(NULL != out, ERRSTR_NULLP);                                        \
    for (;;) {                                                                  \
        chd = (uint64_t)ATOMIC64_GET(&q->chead);                                \
        idx = (uint32_t)(chd & q->idxmask);                                     \
        blk = &q->blocks[idx];                                                  \
        /* 读序 reserved → committed → allocated 不可颠倒,换了会读到没写完的槽 */  \
        resv = (uint64_t)ATOMIC64_GET(&blk->reserved);                          \
        if (BBQ_OFF(resv) < q->blksz) {                                         \
            comm = (uint64_t)ATOMIC64_GET(&blk->committed);                     \
            if (BBQ_OFF(comm) == BBQ_OFF(resv)) {                               \
                alloc = (uint64_t)ATOMIC64_GET(&blk->allocated);                \
                return (BBQ_OFF(alloc) != BBQ_OFF(comm)) ? 1 : ERR_FAILED;      \
            }                                                                   \
            /* 本块没填满时,得等已抢的槽全提交完,前缀才是实数据 */                  \
            if (BBQ_OFF(comm) != q->blksz) {                                    \
                alloc = (uint64_t)ATOMIC64_GET(&blk->allocated);                \
                if (BBQ_OFF(alloc) != BBQ_OFF(comm)) {                          \
                    return 1;                                                   \
                }                                                               \
            }                                                                   \
            if (!ATOMIC64_CAS(&blk->reserved, (atomic64_t)resv,                 \
                              (atomic64_t)(resv + 1))) {                        \
                continue;                                                       \
            }                                                                   \
            *out = q->cells[(size_t)idx * q->blksz + BBQ_OFF(resv)];            \
            ATOMIC64_ADD(&blk->consumed, 1);/* 顺序承重:必须在拷贝之后 */         \
            return ERR_OK;                                                      \
        }                                                                       \
        nblk = &q->blocks[(chd + 1) & q->idxmask];                              \
        comm = (uint64_t)ATOMIC64_GET(&nblk->committed);                        \
        if (BBQ_VSN(comm) != chd + 1) {                                         \
            if ((uint64_t)ATOMIC64_GET(&q->chead) != chd) {                     \
                continue;/* 头被别的消费者推过,刚才那次是假空 */                   \
            }                                                                   \
            return ERR_FAILED;                                                  \
        }                                                                       \
        _bbq_maxset(&nblk->consumed, BBQ_CUR(chd + 1, 0));/* consumed 必须先 */  \
        _bbq_maxset(&nblk->reserved, BBQ_CUR(chd + 1, 0));                      \
        _bbq_maxset(&q->chead, chd + 1);                                        \
    }                                                                           \
}                                                                               \
/* 出队(单消费者):consumed / chead 只有本线程写,所以整条路径没有 CAS 也没有 FAA。     \
   生产者靠 consumed 判断块能否回收,写 consumed 的 release 不能降级。                \
   reserved 只用于多消费者间预订,单消费者路径不维护它,故同一队列                   \
   不得混用 pop 与 pop_sc。三态同 name##_pop,读序中 consumed 顶替 reserved 的位置 */  \
static inline int32_t name##_pop_sc(name *q, T *out) {                          \
    name##_block *blk;                                                          \
    name##_block *nblk;                                                         \
    uint64_t chd, comm, alloc, cons;                                            \
    uint32_t idx;                                                               \
    ASSERTAB(NULL != out, ERRSTR_NULLP);                                        \
    for (;;) {                                                                  \
        chd = (uint64_t)ATOMIC64_GET(&q->chead);                                \
        idx = (uint32_t)(chd & q->idxmask);                                     \
        blk = &q->blocks[idx];                                                  \
        cons = (uint64_t)ATOMIC64_GET(&blk->consumed);                          \
        if (BBQ_OFF(cons) < q->blksz) {                                         \
            comm = (uint64_t)ATOMIC64_GET(&blk->committed);                     \
            if (BBQ_OFF(comm) == BBQ_OFF(cons)) {                               \
                alloc = (uint64_t)ATOMIC64_GET(&blk->allocated);                \
                return (BBQ_OFF(alloc) != BBQ_OFF(comm)) ? 1 : ERR_FAILED;      \
            }                                                                   \
            if (BBQ_OFF(comm) != q->blksz) {                                    \
                alloc = (uint64_t)ATOMIC64_GET(&blk->allocated);                \
                if (BBQ_OFF(alloc) != BBQ_OFF(comm)) {                          \
                    return 1;                                                   \
                }                                                               \
            }                                                                   \
            *out = q->cells[(size_t)idx * q->blksz + BBQ_OFF(cons)];            \
            ATOMIC64_SET_RELEASE(&blk->consumed, (atomic64_t)(cons + 1));       \
            return ERR_OK;                                                      \
        }                                                                       \
        nblk = &q->blocks[(chd + 1) & q->idxmask];                              \
        comm = (uint64_t)ATOMIC64_GET(&nblk->committed);                        \
        if (BBQ_VSN(comm) != chd + 1) {                                         \
            return ERR_FAILED;                                                  \
        }                                                                       \
        ATOMIC64_SET_RELEASE(&nblk->consumed, (atomic64_t)BBQ_CUR(chd + 1, 0)); \
        ATOMIC64_SET_RELEASE(&q->chead, (atomic64_t)(chd + 1));                 \
    }                                                                           \
}                                                                               \
/* 批量出队(单消费者):一段连续已提交的槽一次搬走,consumed 只写一次。               \
   判据与取数顺序同 name##_pop_sc。rtn 口径同 MPQ_DECL 的同名函数:只分取满与没取满,\
   不区分真空与在途 */                                                           \
static inline uint32_t name##_pop_sc_batch(name *q, T *out, uint32_t max, int32_t *rtn) {\
    name##_block *blk;                                                          \
    name##_block *nblk;                                                         \
    uint64_t chd, comm, alloc, cons;                                            \
    uint32_t idx, orr, oc, k;                                                   \
    uint32_t got = 0;                                                           \
    ASSERTAB(NULL != out, ERRSTR_NULLP);                                        \
    while (got < max) {                                                         \
        chd = (uint64_t)ATOMIC64_GET(&q->chead);                                \
        idx = (uint32_t)(chd & q->idxmask);                                     \
        blk = &q->blocks[idx];                                                  \
        cons = (uint64_t)ATOMIC64_GET(&blk->consumed);                          \
        orr = BBQ_OFF(cons);                                                    \
        if (orr < q->blksz) {                                                   \
            comm = (uint64_t)ATOMIC64_GET(&blk->committed);                     \
            oc = BBQ_OFF(comm);                                                 \
            if (oc == orr) {                                                    \
                break;                                                          \
            }                                                                   \
            if (oc != q->blksz) {                                               \
                alloc = (uint64_t)ATOMIC64_GET(&blk->allocated);                \
                if (BBQ_OFF(alloc) != oc) {                                     \
                    break;                                                      \
                }                                                               \
            }                                                                   \
            k = oc - orr;                                                       \
            if (k > max - got) {                                                \
                k = max - got;                                                  \
            }                                                                   \
            memcpy(out + got, &q->cells[(size_t)idx * q->blksz + orr],          \
                   (size_t)k * sizeof(T));                                      \
            ATOMIC64_SET_RELEASE(&blk->consumed, (atomic64_t)(cons + k));       \
            got += k;                                                           \
            continue;                                                           \
        }                                                                       \
        nblk = &q->blocks[(chd + 1) & q->idxmask];                              \
        comm = (uint64_t)ATOMIC64_GET(&nblk->committed);                        \
        if (BBQ_VSN(comm) != chd + 1) {                                         \
            break;                                                              \
        }                                                                       \
        ATOMIC64_SET_RELEASE(&nblk->consumed, (atomic64_t)BBQ_CUR(chd + 1, 0)); \
        ATOMIC64_SET_RELEASE(&q->chead, (atomic64_t)(chd + 1));                 \
    }                                                                           \
    SET_PTR(rtn, (got >= max) ? ERR_OK : ERR_FAILED);                           \
    return got;                                                                 \
}

// 原子取最大。没有原子 MAX 指令,拿 CAS 凑;游标高位是单调增的 epoch、低位是同代内单调增的
// off,所以直接按无符号比大小就是正确的推进顺序。多个线程同时推进同一个块时靠它幂等
static inline void _bbq_maxset(atomic64_t *p, uint64_t val) {
    uint64_t cur = (uint64_t)ATOMIC64_GET(p);
    while (cur < val) {
        if (ATOMIC64_CAS(p, (atomic64_t)cur, (atomic64_t)val)) {
            return;
        }
        cur = (uint64_t)ATOMIC64_GET(p);
    }
}
// 选块数。块头固定 3 条 cache line,先按"块头开销不超过 10%"定出每块至少要装多少槽,
// 再看这个容量能切出几块,夹在 [4, 16]:少于 4 块回收粒度太粗,多于 16 块推块的开销摊不薄。
// 容量太小时下限 4 优先,那时块头占比压不下来,认了
static inline uint32_t _bbq_nblk(uint32_t capacity, uint32_t elsize) {
    uint32_t nblk;
    uint32_t least = (uint32_t)(30 * CACHELINE_SIZE) / elsize;
    least = pow2_ceil(least < 16 ? 16 : least);
    nblk = (capacity >= least) ? (capacity / least) : 1;
    if (nblk < 4) {
        nblk = 4;
    }
    return (nblk > 16) ? 16 : nblk;
}

#endif//BBQ_H_
