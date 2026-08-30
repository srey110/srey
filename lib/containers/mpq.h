#ifndef MPQ_H_
#define MPQ_H_

#include "base/structs.h"

//无锁多生产者有界队列 (Multi-Producer Queue, Lock-Free)
//生产者侧固定多线程 CAS 抢 enq.v；消费者侧由调用方约定：
//  - mpq_pop    多消费者安全（内部 CAS 抢 deq.v）
//  - mpq_pop_sc 单消费者优化（无 CAS，要求调用方保证仅一个线程调用 pop_sc）
//单元素：序列号 + 柔性数据区（长度 = init 时的 elsize，按值存任意定长元素）
typedef struct mpq_cell {
    atomic_t  sequence;
    char      data[];
} mpq_cell;
//无锁多生产者队列上下文
typedef struct mpq_ctx {
    uint32_t      capacity; //队列容量，必须为 2 的幂
    uint32_t      mask;     //capacity - 1，用于快速取模
    uint32_t      elsize;   //单元素字节数（init 时指定）
    uint32_t      stride;   //每槽位字节数 = ROUND_UP(sizeof(atomic_t)+elsize, 8)
    char          *cells;   //槽位数组基址（按 stride 步进寻址，不可用下标索引）
    char          _pad0[CACHELINE_SIZE];//把上面这几个只读字段与 enq 隔开：每次 push 的 CAS
                            //都会让别的核重读 mask/stride/elsize/cells，而 push 和 pop 每次都要用它们
    atomic_aln_t  enq;      //入队位置计数器，多生产者 CAS 抢（与 deq 各占一条 cache line）
    atomic_aln_t  deq;      //出队位置计数器
} mpq_ctx;
/// <summary>
/// 初始化队列
/// </summary>
/// <param name="q">mpq_ctx</param>
/// <param name="elsize">单元素字节数（按值存储，须 大于 0）</param>
/// <param name="capacity">期望容量，0 则使用默认值，非 2 的幂自动向上取整</param>
void mpq_init(mpq_ctx *q, size_t elsize, uint32_t capacity);
/// <summary>
/// 释放队列内部内存，不释放 q 本身
/// </summary>
/// <param name="q">mpq_ctx</param>
void mpq_free(mpq_ctx *q);
/// <summary>
/// 非阻塞入队
/// </summary>
/// <param name="q">mpq_ctx</param>
/// <param name="data">指向待入队元素的指针，不得为 NULL（拷贝 elsize 字节）</param>
/// <returns>ERR_OK 成功，ERR_FAILED 队列已满</returns>
int32_t mpq_trypush(mpq_ctx *q, const void *data);
/// <summary>
/// 出队（多消费者）
/// </summary>
/// <param name="q">mpq_ctx</param>
/// <param name="out">出参：接收出队元素的缓冲（至少 elsize 字节），仅 ERR_OK 时有效</param>
/// <returns>ERR_OK 出队成功；ERR_FAILED 队列确实为空；
/// 1 队列看似空，但有槽位已被生产者抢占、尚未发布——更早入队的元素还在路上。
/// 只关心"有没有取到"的调用方按 ERR_OK 判即可，两种非 OK 都当空处理；
/// 队列之外另有一层数据源的调用方（如 fsqu 的溢出层）必须区分：报 1 时那一层里
/// 更晚入队的元素不能抢在这个在途元素之前取，否则顺序就反了</returns>
int32_t mpq_pop(mpq_ctx *q, void *out);
/// <summary>
/// 出队（单消费者）：仅允许单一消费者线程调用；同一队列上 pop 与 pop_sc 也不可混用
/// </summary>
/// <param name="q">mpq_ctx</param>
/// <param name="out">出参：接收出队元素的缓冲（至少 elsize 字节），仅 ERR_OK 时有效</param>
/// <returns>三态语义同 mpq_pop：ERR_OK 成功；ERR_FAILED 确实为空；1 有在途元素（区分义务见彼处）</returns>
int32_t mpq_pop_sc(mpq_ctx *q, void *out);
/// <summary>
/// 返回当前队列元素数量的近似值：只会高估不会低估(上限 capacity)，不会把有元素报成 0
/// ——调用方靠它判"还有没有活要干"，低估就是漏唤醒
/// </summary>
/// <param name="q">mpq_ctx</param>
/// <returns>元素数量，取值 [0, capacity]</returns>
uint32_t mpq_size(mpq_ctx *q);
/// <summary>
/// 返回队列最大容量
/// </summary>
/// <param name="q">mpq_ctx</param>
/// <returns>最大容量</returns>
static inline uint32_t mpq_capacity(const mpq_ctx *q) { return q->capacity; }

#endif//MPQ_H_
