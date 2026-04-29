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
/// <param name="lv">LOG_LEVEL</param>
void log_setlv(LOG_LEVEL lv);
/// <summary>
/// 输出一条日志，低于当前日志级别时直接忽略
/// </summary>
/// <param name="lv">日志级别</param>
/// <param name="fmt">格式化字符串</param>
/// <param name="...">变参</param>
void slog(int32_t lv, const char *fmt, ...);

#endif//LOG_H_
