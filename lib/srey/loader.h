#ifndef LOADER_H_
#define LOADER_H_

#include "srey/spub.h"

/// <summary>
/// 任务调度初始化
/// </summary>
/// <param name="nnet">网络线程数, 0 cpu核心数</param>
/// <param name="nworker">工作线程数, 0 cpu核心数</param>
/// <returns>loader_ctx</returns>
loader_ctx *loader_init(uint16_t nnet, uint16_t nworker);
/// <summary>
/// 任务调度释放
/// </summary>
/// <param name="loader">loader_ctx</param>
void loader_free(loader_ctx *loader);

#endif//LOADER_H_
