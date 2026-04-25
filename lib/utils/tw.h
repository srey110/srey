#ifndef TW_H_
#define TW_H_

#include "thread/thread.h"
#include "thread/spinlock.h"
#include "thread/mutex.h"
#include "thread/cond.h"
#include "utils/timer.h"
#include "base/structs.h"

#define TVN_BITS (6)                                                   //高精度轮（tv2~tv5）每级位数
#define TVR_BITS (8)                                                   //最低精度轮（tv1）位数
#define TVN_SIZE (1 << TVN_BITS)                                       //高精度轮每级槽数（64）
#define TVR_SIZE (1 << TVR_BITS)                                       //最低精度轮槽数（256）
#define TVN_MASK (TVN_SIZE - 1)                                        //高精度轮索引掩码
#define TVR_MASK (TVR_SIZE - 1)                                        //最低精度轮索引掩码
#define INDEX(N) ((ctx->jiffies >> (TVR_BITS + (N) * TVN_BITS)) & TVN_MASK) //计算第 N 级高精度轮的当前槽索引
typedef void(*tw_cb)(ud_cxt *ud);   //超时回调函数类型
typedef struct tw_node_ctx {
    struct tw_node_ctx *next;
    tw_cb _cb;          //超时触发的回调函数
    free_cb _freecb;    //用户数据释放回调
    ud_cxt ud;          //用户数据
    uint64_t expires;   //到期时间（毫秒绝对时间）
}tw_node_ctx;
typedef struct tw_slot_ctx {
    tw_node_ctx *head;  //链表头节点
    tw_node_ctx *tail;  //链表尾节点
}tw_slot_ctx;
typedef struct tw_ctx {
    volatile int32_t exit;  //退出标志，非零时轮线程退出
    uint64_t jiffies;       //当前时间轮逻辑时钟（毫秒）
    spin_ctx spin;          //保护 reqadd 的自旋锁
    pthread_t thtw;         //时间轮工作线程
    timer_ctx timer;        //高精度计时器
    /* mutex+cond 用于精确睡眠：tw_add 写入后唤醒轮线程，
     * 替代原先每 1ms 无条件 USLEEP(1000) 的忙等待。 */
    mutex_ctx mu;
    cond_ctx  cond;
    tw_slot_ctx reqadd;         //外部新增请求暂存槽（由 spin 保护）
    tw_slot_ctx tv1[TVR_SIZE];  //最低精度轮（精度 1ms，范围 256ms）
    tw_slot_ctx tv2[TVN_SIZE];  //第 2 级精度轮
    tw_slot_ctx tv3[TVN_SIZE];  //第 3 级精度轮
    tw_slot_ctx tv4[TVN_SIZE];  //第 4 级精度轮
    tw_slot_ctx tv5[TVN_SIZE];  //第 5 级精度轮（最大超时约 2^32 ms）
}tw_ctx;
/// <summary>
/// 时间轮初始化
/// </summary>
/// <param name="ctx">tw_ctx</param>
void tw_init(tw_ctx *ctx);
/// <summary>
/// 时间轮释放
/// </summary>
/// <param name="ctx">tw_ctx</param>
void tw_free(tw_ctx *ctx);
/// <summary>
/// 添加计时任务
/// </summary>
/// <param name="ctx">tw_ctx</param>
/// <param name="timeout">超时 毫秒</param>
/// <param name="_cb">超时回调函数</param>
/// <param name="_freecb">参数释放函数</param>
/// <param name="ud">参数</param>
void tw_add(tw_ctx *ctx, const uint32_t timeout, tw_cb _cb, free_cb _freecb, ud_cxt *ud);

#endif//TW_H_
