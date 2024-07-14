#ifndef LOG_H_
#define LOG_H_

#include "base/macro.h"

/// <summary>
/// 日志初始化
/// </summary>
/// <param name="file">输出句柄</param>
void log_init(FILE *file);
/// <summary>
/// 设置日志级别
/// </summary>
/// <param name="lv">LOG_LEVEL</param>
void log_setlv(LOG_LEVEL lv);

#endif//LOG_H_
