#ifndef FSQU_H_
#define FSQU_H_

#include "containers/queue.h"
#include "thread/spinlock.h"
// 快路径的实现由 FSQU_FAST_MODEL 选,三家的接口签名相同,换一行 DECL 即可。
// 这个别名是在 FSQU_DECL 的调用点展开的,不是定义点——别 #undef,undef 了每个
// FSQU_DECL(...) 都会原样吐出 FSQU_FAST_DECL(...) 而不是报宏错
#if 2 == FSQU_FAST_MODEL
    #include "containers/bbq.h"
    #define FSQU_FAST_DECL BBQ_DECL
#elif 1 == FSQU_FAST_MODEL
    #include "containers/mpq.h"
    #define FSQU_FAST_DECL MPQ_DECL
#elif 0 != FSQU_FAST_MODEL
    /* 不挡的话:FSQU_FAST_DECL 没定义,而下面 #if FSQU_FAST_MODEL 照样成立,
       报错会散到每个 FSQU_DECL(...) 调用点上,看不出是这里写错了 */
    #error "FSQU_FAST_MODEL must be 0 (queue+spin), 1 (mpq) or 2 (bbq)"
#endif

// 平台自适应队列:FSQU_FAST_MODEL 选快路径——0 = queue+spin(整条队列一把自旋锁),
// 1 = mpq(无锁有界环),2 = bbq(无锁分块环);后两者满了降级到无界溢出层。
// 三种后端的接口签名一致,调用方不必关心;例外是选 2 时容量上限 2^20,超了 init 就中止(见 bbq.h)。
//
// 典型用法：
//   FSQU_DECL(msg_fsqu, message_ctx)
//   msg_fsqu q; msg_fsqu_init(&q, 1024);
//   message_ctx m = { 0 };
//   msg_fsqu_push(&q, &m);                    // 永不失败
//   message_ctx out;
//   if (ERR_OK == msg_fsqu_pop_sc(&q, &out)) { ... }
//   msg_fsqu_free(&q);
//
// 元素一律按指针传(同快路径那三家)。

#define FSQU_DEFAULT_CAP 1024 // 默认容量;各后端共用一个默认值

// 入参写 T const * 而不是 const T *：T 是指针类型(如 void *)时，后者会被解析成指向 const 的指针，调用方传普通指针就告警。
// FSQU_DECL(name, T)：name 生成的类型名，T 元素类型，按 FSQU_FAST_MODEL 展开成下面两种本体之一。
// 产线只用 FSQU_DECL；要在同一个二进制里并排比几种后端时才直接用本体：
// FSQU_RING_DECL 的 FASTDECL 传 MPQ_DECL 或 BBQ_DECL(头文件由调用方自己 include)，FSQU_SPIN_DECL 即 0 号后端
#define FSQU_RING_DECL(name, T, FASTDECL)                                    \
QUE_DECL(name##_ovf, T)                                                         \
FASTDECL(name##_fast, T)                                                        \
typedef struct {                                                                \
    atomic_t novf;      /* 溢出层元素数镜像，锁外无锁读(push 粘滞判定 / pop、size 快路径) */\
    spin_ctx lck;       /* 保护 qu */                                            \
    name##_ovf qu;      /* 快路径满时的溢出层 */                                  \
    name##_fast fast;   /* 快路径(无锁环)，满则降级到 qu */                        \
} name;                                                                         \
/* capacity 为快路径容量,0 取 FSQU_DEFAULT_CAP;溢出层首次溢出才分配 */          \
static inline void name##_init(name *fsqu, uint32_t capacity) {                 \
    capacity = (0 == capacity) ? FSQU_DEFAULT_CAP : capacity;                    \
    ATOMIC_SET_RELAXED(&fsqu->novf, 0);                                                 \
    spin_init(&fsqu->lck, SPIN_CNT);                                            \
    /* 溢出层走延迟分配:首次溢出才由 push 申请缓冲 */                              \
    name##_ovf_init(&fsqu->qu, 0);                                              \
    name##_fast_init(&fsqu->fast, capacity);                                    \
}                                                                               \
/* 释放快路径、溢出层与锁 */                                                    \
static inline void name##_free(name *fsqu) {                                    \
    name##_fast_free(&fsqu->fast);                                              \
    name##_ovf_free(&fsqu->qu);                                                 \
    spin_free(&fsqu->lck);                                                      \
}                                                                               \
/* 非阻塞入队:队满不阻塞、不扩容、不落溢出层。溢出层非空期间一律拒,免新元素排到      \
   更早的溢出元素之前。这道守卫是 best-effort——读 novf 与随后的入队不是一个原子步 */ \
static inline int32_t name##_trypush(name *fsqu, T const *data) {               \
    if (0 != ATOMIC_GET(&fsqu->novf)) {                                         \
        return ERR_FAILED;                                                      \
    }                                                                           \
    return name##_fast_trypush(&fsqu->fast, data);                              \
}                                                                               \
/* 入队:永不阻塞、永不失败。快路径满时降级到无界溢出层,该层只增不减、峰值保留到 free。\
   这是为消除自投递死锁有意接受的取舍 */                                          \
static inline void name##_push(name *fsqu, T const *data) {                     \
    if (0 == ATOMIC_GET(&fsqu->novf)                                            \
        && ERR_OK == name##_fast_trypush(&fsqu->fast, data)) {                  \
        return;                                                                 \
    }                                                                           \
    spin_lock(&fsqu->lck);                                                      \
    /* 置位排在入队之前:生产者侧免锁读 novf,排在后面会留出"已进溢出层、novf 仍为 0"的窗口 */\
    ATOMIC_ADD(&fsqu->novf, 1);                                                 \
    name##_ovf_push(&fsqu->qu, data);                                          \
    spin_unlock(&fsqu->lck);                                                    \
}                                                                               \
/* 批量入队,语义同 push */                                                      \
static inline void name##_push_batch(name *fsqu, T const *data, uint32_t count) {\
    uint32_t i = 0;                                                             \
    uint32_t nleft;                                                             \
    /* 粘滞降级:溢出层非空时整批直落溢出,不与更早的溢出元素交错 */                   \
    if (0 == ATOMIC_GET(&fsqu->novf)) {                                         \
        while (i < count                                                        \
               && ERR_OK == name##_fast_trypush(&fsqu->fast, data + i)) {       \
            i++;                                                                \
        }                                                                       \
    }                                                                           \
    if (i >= count) {                                                           \
        return;                                                                 \
    }                                                                           \
    nleft = count - i;                                                          \
    spin_lock(&fsqu->lck);                                                      \
    ATOMIC_ADD(&fsqu->novf, (atomic_t)nleft);/* 同 push:先置位再入队 */           \
    for (; i < count; i++) {                                                    \
        name##_ovf_push(&fsqu->qu, &data[i]);                                  \
    }                                                                           \
    spin_unlock(&fsqu->lck);                                                    \
}                                                                               \
/* 快路径取完后从溢出层续取补齐到 out[*n..max);*fastrtn 回写为锁内那次查快路径的结果。    \
   锁内必须先把快路径查到底再取溢出层:放溢出层要持锁,同一生产者更早进快路径的元素锁内必然可见,\
   凭锁外那次"快路径空"直接排溢出层会乱序。快路径在途(1)就不碰溢出层 */               \
static inline void name##_ovf_drain(name *fsqu, T *out, uint32_t max,           \
                                    uint32_t *n, int32_t *fastrtn, int32_t sc) { \
    int32_t k;                                                                  \
    if (*n >= max || 1 == *fastrtn || 0 == ATOMIC_GET(&fsqu->novf)) {           \
        return;                                                                 \
    }                                                                           \
    spin_lock(&fsqu->lck);                                                      \
    while (*n < max                                                             \
           && ERR_OK == (*fastrtn = sc ? name##_fast_pop_sc(&fsqu->fast, out + *n)\
                                       : name##_fast_pop(&fsqu->fast, out + *n))) {\
        (*n)++;                                                                 \
    }                                                                           \
    if (*n < max                                                                \
        && 1 != *fastrtn) {                                                     \
        k = (int32_t)name##_ovf_pop_batch(&fsqu->qu, out + *n, max - *n);       \
        if (0 != k) {                                                           \
            *n += (uint32_t)k;                                                  \
            ATOMIC_ADD_RELAXED(&fsqu->novf, -k);/* 批量一次扣减,省 k-1 次原子操作 */       \
        }                                                                       \
    }                                                                           \
    spin_unlock(&fsqu->lck);                                                    \
}                                                                               \
/* 批量出队主体,pop_batch / pop_sc_batch 共用;sc 传字面量,分支被常量折叠。          \
   批量的 rtn 不分真空与在途,要续取溢出层时由 ovf_drain 锁内那次单条查询给出准确结果 */ \
static inline uint32_t name##_pop_batch_impl(name *fsqu, T *out, uint32_t max, int32_t sc) {\
    int32_t rtn = ERR_FAILED;                                                   \
    /* 只有单消费者侧走批量:多消费者一次抢一批槽,消费者一多就几乎抢不到,不如逐条 */  \
    uint32_t n = sc ? name##_fast_pop_sc_batch(&fsqu->fast, out, max, &rtn) : 0; \
    if (!sc) {                                                                  \
        while (n < max                                                          \
               && ERR_OK == (rtn = name##_fast_pop(&fsqu->fast, out + n))) {    \
            n++;                                                                \
        }                                                                       \
    }                                                                           \
    name##_ovf_drain(fsqu, out, max, &n, &rtn, sc);                             \
    return n;                                                                   \
}                                                                               \
/* 单条出队主体,pop / pop_sc 共用;sc 传字面量 */                                  \
static inline int32_t name##_pop_impl(name *fsqu, T *out, int32_t sc) {         \
    uint32_t n = 0;                                                             \
    int32_t rtn = sc ? name##_fast_pop_sc(&fsqu->fast, out) : name##_fast_pop(&fsqu->fast, out);\
    if (ERR_OK == rtn) {                                                        \
        return ERR_OK;                                                          \
    }                                                                           \
    name##_ovf_drain(fsqu, out, 1, &n, &rtn, sc);                               \
    return (0 != n) ? ERR_OK : rtn;                                             \
}                                                                               \
/* 三态返回,同 MPQ_DECL 的 pop:ERR_OK 成功;ERR_FAILED 确实为空;                    \
   1 有元素已被生产者抢占、尚未发布。靠 size 决定睡不睡的调用方必须区分后两者 */      \
static inline int32_t name##_pop(name *fsqu, T *out) {                          \
    return name##_pop_impl(fsqu, out, 0);                                       \
}                                                                               \
/* 批量出队(多消费者安全),返回取到的个数 */                                     \
static inline uint32_t name##_pop_batch(name *fsqu, T *out, uint32_t max) {     \
    return name##_pop_batch_impl(fsqu, out, max, 0);                            \
}                                                                               \
/* 单消费者版:仅允许单一消费者线程调用,不可与 pop 混用 */                          \
static inline int32_t name##_pop_sc(name *fsqu, T *out) {                       \
    return name##_pop_impl(fsqu, out, 1);                                       \
}                                                                               \
/* 批量出队(单消费者),返回取到的个数 */                                         \
static inline uint32_t name##_pop_sc_batch(name *fsqu, T *out, uint32_t max) {  \
    return name##_pop_batch_impl(fsqu, out, max, 1);                            \
}                                                                               \
/* 含溢出层:调用方以此判空决定重调度/休眠,漏算会让溢出元素滞留。只高估不低估 */       \
static inline uint32_t name##_size(name *fsqu) {                                \
    return name##_fast_size(&fsqu->fast) + (uint32_t)ATOMIC_GET(&fsqu->novf);   \
}                                                                               \
/* 是否为空,含溢出层,口径同 size */                                             \
static inline int32_t name##_empty(name *fsqu) {                                \
    /* 读序与 size 一致(先快路径后 novf),短路排在常见的"有活"那一侧 */            \
    if (!name##_fast_empty(&fsqu->fast)) {                                      \
        return 0;                                                               \
    }                                                                           \
    return 0 == ATOMIC_GET(&fsqu->novf);                                        \
}                                                                               \
/* 同 FSQU_SPIN_DECL 的 empty_fast;本后端 empty 本来就不拿锁 */                 \
static inline int32_t name##_empty_fast(name *fsqu) {                           \
    return name##_empty(fsqu);                                                  \
}                                                                               \
/* 同 FSQU_SPIN_DECL 的 size_fast;本后端 size 本来就不拿锁 */                  \
static inline uint32_t name##_size_fast(name *fsqu) {                           \
    return name##_size(fsqu);                                                   \
}                                                                               \
/* 元素字节数 sizeof(T) */                                                      \
static inline uint32_t name##_elsize(const name *fsqu) { (void)fsqu; return (uint32_t)sizeof(T); }\
/* 快路径固定容量(降级阈值,不含无界的溢出层) */                                    \
static inline uint32_t name##_capacity(name *fsqu) {                            \
    return name##_fast_capacity(&fsqu->fast);                                   \
}
#define FSQU_SPIN_DECL(name, T)                                                \
QUE_DECL(name##_ovf, T)                                                         \
typedef struct {                                                                \
    atomic_t nhint;     /* 元素数镜像,锁内每次改完照 qu 写一份、锁外只读,读到的可能稍旧 */\
    spin_ctx lck;       /* 保护 qu */                                            \
    name##_ovf qu;      /* 主队列 */                                             \
} name;                                                                         \
/* capacity 为那一条队列的初始容量,0 取 FSQU_DEFAULT_CAP */                     \
static inline void name##_init(name *fsqu, uint32_t capacity) {                 \
    capacity = (0 == capacity) ? FSQU_DEFAULT_CAP : capacity;                    \
    ATOMIC_SET_RELAXED(&fsqu->nhint, 0);                                        \
    spin_init(&fsqu->lck, SPIN_CNT);                                            \
    name##_ovf_init(&fsqu->qu, capacity);                                       \
}                                                                               \
/* 释放队列与锁 */                                                              \
static inline void name##_free(name *fsqu) {                                    \
    name##_ovf_free(&fsqu->qu);                                                 \
    spin_free(&fsqu->lck);                                                      \
}                                                                               \
/* 满了返回 ERR_FAILED,不扩容 */                                                \
static inline int32_t name##_trypush(name *fsqu, T const *data) {               \
    int32_t rtn;                                                                \
    spin_lock(&fsqu->lck);                                                      \
    rtn = name##_ovf_trypush(&fsqu->qu, data);                                 \
    ATOMIC_SET_RELAXED(&fsqu->nhint, (atomic_t)name##_ovf_size(&fsqu->qu));     \
    spin_unlock(&fsqu->lck);                                                    \
    return rtn;                                                                 \
}                                                                               \
/* 入队,满了自动扩容,永不失败 */                                                \
static inline void name##_push(name *fsqu, T const *data) {                     \
    spin_lock(&fsqu->lck);                                                      \
    name##_ovf_push(&fsqu->qu, data);                                          \
    ATOMIC_SET_RELAXED(&fsqu->nhint, (atomic_t)name##_ovf_size(&fsqu->qu));     \
    spin_unlock(&fsqu->lck);                                                    \
}                                                                               \
/* 批量入队,语义同 push */                                                      \
static inline void name##_push_batch(name *fsqu, T const *data, uint32_t count) {\
    uint32_t i;                                                                 \
    spin_lock(&fsqu->lck);                                                      \
    for (i = 0; i < count; i++) {                                               \
        name##_ovf_push(&fsqu->qu, &data[i]);                                  \
    }                                                                           \
    ATOMIC_SET_RELAXED(&fsqu->nhint, (atomic_t)name##_ovf_size(&fsqu->qu));     \
    spin_unlock(&fsqu->lck);                                                    \
}                                                                               \
/* 三态里的 1 只有无锁环后端(mpq / bbq)会产生,这里只返 ERR_OK / ERR_FAILED */       \
static inline int32_t name##_pop(name *fsqu, T *out) {                          \
    T *elem;                                                                    \
    spin_lock(&fsqu->lck);                                                      \
    elem = name##_ovf_pop(&fsqu->qu);                                           \
    if (NULL == elem) {                                                         \
        spin_unlock(&fsqu->lck);                                                \
        return ERR_FAILED;                                                      \
    }                                                                           \
    *out = *elem;                                                               \
    ATOMIC_SET_RELAXED(&fsqu->nhint, (atomic_t)name##_ovf_size(&fsqu->qu));     \
    spin_unlock(&fsqu->lck);                                                    \
    return ERR_OK;                                                              \
}                                                                               \
/* 批量出队,返回取到的个数 */                                                   \
static inline uint32_t name##_pop_batch(name *fsqu, T *out, uint32_t max) {     \
    uint32_t n;                                                                 \
    spin_lock(&fsqu->lck);                                                      \
    n = name##_ovf_pop_batch(&fsqu->qu, out, max);                              \
    if (0 != n) {                                                               \
        ATOMIC_SET_RELAXED(&fsqu->nhint, (atomic_t)name##_ovf_size(&fsqu->qu)); \
    }                                                                           \
    spin_unlock(&fsqu->lck);                                                    \
    return n;                                                                   \
}                                                                               \
/* 同 pop,本后端没有单消费者优化 */                                             \
static inline int32_t name##_pop_sc(name *fsqu, T *out) {                       \
    return name##_pop(fsqu, out);                                               \
}                                                                               \
/* 同 pop_batch */                                                              \
static inline uint32_t name##_pop_sc_batch(name *fsqu, T *out, uint32_t max) {  \
    return name##_pop_batch(fsqu, out, max);                                    \
}                                                                               \
/* 元素数 */                                                                    \
static inline uint32_t name##_size(name *fsqu) {                                \
    uint32_t n;                                                                 \
    spin_lock(&fsqu->lck);                                                      \
    n = name##_ovf_size(&fsqu->qu);                                             \
    spin_unlock(&fsqu->lck);                                                    \
    return n;                                                                   \
}                                                                               \
/* 是否为空 */                                                                  \
static inline int32_t name##_empty(name *fsqu) {                                \
    int32_t rtn;                                                                \
    spin_lock(&fsqu->lck);                                                      \
    rtn = name##_ovf_empty(&fsqu->qu);                                          \
    spin_unlock(&fsqu->lck);                                                    \
    return rtn;                                                                 \
}                                                                               \
/* 不拿锁的近似判空,读到的计数可能稍旧,但本线程自己 push 的总能看见。           \
   给空转轮询与"只在乎本线程推过没有"的判定用;决定睡不睡的那次复查仍要用 empty */  \
static inline int32_t name##_empty_fast(name *fsqu) {                           \
    return 0 == ATOMIC_GET(&fsqu->nhint);                                       \
}                                                                               \
/* 不拿锁的近似元素数,读到的可能稍旧,口径同 empty_fast */                       \
static inline uint32_t name##_size_fast(name *fsqu) {                           \
    return (uint32_t)ATOMIC_GET(&fsqu->nhint);                                  \
}                                                                               \
/* 元素字节数 sizeof(T) */                                                      \
static inline uint32_t name##_elsize(const name *fsqu) { (void)fsqu; return (uint32_t)sizeof(T); }\
/* 队列当前容量,push 扩容后会变大 */                                            \
static inline uint32_t name##_capacity(name *fsqu) {                            \
    uint32_t cap;                                                               \
    spin_lock(&fsqu->lck);                                                      \
    cap = name##_ovf_capacity(&fsqu->qu);                                        \
    spin_unlock(&fsqu->lck);                                                    \
    return cap;                                                                 \
}

#if FSQU_FAST_MODEL
    #define FSQU_DECL(name, T) FSQU_RING_DECL(name, T, FSQU_FAST_DECL)
#else
    #define FSQU_DECL(name, T) FSQU_SPIN_DECL(name, T)
#endif

#endif//FSQU_H_
