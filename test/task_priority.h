#ifndef TASK_PRIORITY_H_
#define TASK_PRIORITY_H_

#include "lib.h"

// task_set_priority / task_get_priority 单元测试:
//   1) round-trip: 0/1/8/16 四个边界值 setter→getter 一致
//   2) clamp 上限: 17/100/INT32_MAX 全部 clamp 到 TASK_PRIORITY_MAX (16)
//   3) clamp 下限: -1/-100/INT32_MIN 全部 clamp 到 0
//   4) atomic 读写: 高频 set + get 循环不出现非法中间值 (单线程内即可验证 clamp 正确性)
// 全部 case 通过后将 *ok 置 1。
void task_priority_start(loader_ctx *loader, const char *name, int32_t *ok);

#endif//TASK_PRIORITY_H_
