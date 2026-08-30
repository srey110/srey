#ifndef LOG_H_
#define LOG_H_

#include "base/macro.h"

/// <summary>
/// 日志初始化
/// </summary>
/// <param name="file">输出句柄</param>
/// <param name="capacity">日志队列长度</param>
void log_init(FILE *file, uint32_t capacity);
/// <summary>
/// 停止日志 I/O 线程，刷新并释放资源
/// </summary>
void log_free(void);
/// <summary>
/// 设置日志级别
/// </summary>
/// <param name="lv">log_level</param>
void log_setlv(log_level lv);
/// <summary>
/// 获取当前日志级别
/// </summary>
/// <returns>log_level</returns>
log_level log_getlv(void);

#endif//LOG_H_
