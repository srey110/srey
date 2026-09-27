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
/// <summary>
/// 同 slog，但正文已由调用方拼好：各段按顺序原样拷进日志，不过格式化
/// </summary>
/// <param name="lv">日志级别，参见 log_level</param>
/// <param name="parts">各段起址，不必 NUL 结尾；段内含 NUL 时输出截在第一个 NUL 处（同 %s）</param>
/// <param name="lens">各段字节数</param>
/// <param name="n">段数</param>
void slog_parts(int32_t lv, const char *const *parts, const size_t *lens, uint32_t n);

#endif//LOG_H_
