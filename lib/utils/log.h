#ifndef LOG_H_
#define LOG_H_

#include "base/base.h"

// 日志线程的几个参数；放在头文件是为了让用例直接引用，免得各抄一份
#define LOG_INLINE_SIZE 256 // 正文短于它的内嵌在日志项里，不另分配
#define LOG_POP_BATCH   128 // 日志线程单批出队上限，也是浅睡时攒到多少条就叫醒的上限
#define LOG_LINGER_MS   10 // 日志线程写过行之后浅睡的毫秒数
#define LOG_BATCH_BYTES (64 * ONEK) // 日志文件模式下日志线程攒行的缓冲，装不下的长行直写

/// <summary>
/// 日志初始化。调用之前打的日志静默丢弃；队列满时在打日志的线程上同步写出，不阻塞也不丢
/// </summary>
/// <param name="file">输出句柄；NULL 输出到 stdout，按级别上色</param>
/// <param name="capacity">日志队列长度，0 取 4096</param>
void log_init(FILE *file, uint32_t capacity);
/// <summary>
/// 停止日志 I/O 线程，写完队列里剩下的日志并释放资源。
/// 必须在所有可能打日志的线程都停下(join)之后调用，否则会碰到已释放的队列；之后打的日志静默丢弃
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
