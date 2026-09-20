#ifndef BENCH_WEIGHTS_H_
#define BENCH_WEIGHTS_H_

// loader 的 worker weight 分档在不同 nworker 下的效率与公平。
// 每轮给 N 个 task 各预灌等量消息，一次性放行，量两件事：
//   效率 = 全部处理完的总耗时(换算成 msg/s)
//   公平 = 各 task 完成时刻的离差(最早完成与最晚完成差多少)
// 结果经 LOG_INFO 输出。
void bench_weights(void);

#endif//BENCH_WEIGHTS_H_
