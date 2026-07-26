#include "containers/fsqu.h"

#define FSQU_DEFAULT_CAP  1024

void fsqu_init(fsqu_ctx *fsqu, size_t elsize, uint32_t capacity) {
    ZERO(fsqu, sizeof(fsqu_ctx));// 须最先(清 novf 等计数)，否则会抹掉随后初始化好的锁
    capacity = (0 == capacity ? FSQU_DEFAULT_CAP : capacity);
    spin_init(&fsqu->lck, SPIN_CNT);// 须在 ZERO 之后，否则锁状态被抹
#if FSQU_MPQ
    mpq_init(&fsqu->mpq, elsize, capacity);// 槽位 sequence 须初始化为下标，ZERO 替代不了
    queue_init(&fsqu->qu, (uint32_t)elsize, 0);// 溢出层走延迟分配：首次溢出才由 queue_push 申请缓冲
#else
    queue_init(&fsqu->qu, (uint32_t)elsize, capacity);
#endif
}
void fsqu_free(fsqu_ctx *fsqu) {
#if FSQU_MPQ
    mpq_free(&fsqu->mpq);
#endif
    queue_free(&fsqu->qu);// 从未溢出时 ptr 为 NULL，FREE 宏自带守卫
    spin_free(&fsqu->lck);
}
