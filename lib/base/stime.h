#ifndef STIME_H_
#define STIME_H_

#include "base/os.h"
#include "base/macro_util.h"

/// <summary>
/// 填充timespec
/// </summary>
/// <param name="timeout">struct timespec</param>
/// <param name="ms">毫秒</param>
static inline void fill_timespec(struct timespec *timeout, uint32_t ms) {
    timeout->tv_sec = ms / 1000;
    timeout->tv_nsec = (long)(ms % 1000) * (1000 * 1000);
}
/// <summary>
/// timeofday。注意 Windows 的 struct timeval.tv_sec 是 32 位 long,2038-01-19 后回绕,
/// 这是该结构体自身的容量上限;需要不截断的时间戳请用 nowms / nowsec
/// </summary>
/// <param name="tv">timeval</param>
void timeofday(struct timeval *tv);
/// <summary>
/// 与UTC时差
/// </summary>
/// <returns>分。系统时间换算失败时返 0(按 UTC 处理),不另设错误码</returns>
int32_t timeoffset(void);
/// <summary>
/// 当前时间戳
/// </summary>
/// <returns>毫秒</returns>
uint64_t nowms(void);
/// <summary>
/// 当前时间戳
/// </summary>
/// <returns>秒</returns>
uint64_t nowsec(void);
/// <summary>
/// 格式化输出时间戳；失败时 time[0] = '\0'
/// </summary>
/// <param name="sec">秒</param>
/// <param name="fmt">格式化 %Y-%m-%d %H:%M:%S</param>
/// <param name="time">时间字符串</param>
/// <returns>ERR_OK 成功，ERR_FAILED 失败</returns>
int32_t sectostr(uint64_t sec, const char *fmt, char time[TIME_LENS]);
/// <summary>
/// 格式化输出时间戳；失败时 time[0] = '\0'
/// </summary>
/// <param name="ms">毫秒</param>
/// <param name="fmt">格式化 %Y-%m-%d %H:%M:%S</param>
/// <param name="time">时间字符串</param>
/// <returns>ERR_OK 成功；ERR_FAILED 失败（时间换算失败，或 fmt 太长以致
///     " 000" 毫秒后缀在 TIME_LENS 内装不下——不截断，整体失败）</returns>
int32_t mstostr(uint64_t ms, const char *fmt, char time[TIME_LENS]);
/// <summary>
/// 字符串转时间戳，一律按本地时间 mktime，是否夏令时由 mktime 按本地规则判定。
/// fmt 里的 %z / %Z 只吃掉时区文本，偏移量不生效；认出的时区写法可能改写夏令时标志（数字偏移与 Z / UT / GMT
/// 定为标准时间，EDT 一类定为夏令时，EST 一类与单字母军用时区不改、仍由 mktime 判定）。
/// 带时区的串解析结果会差"串里的偏移 − 本地偏移"秒，要换算的调用方自己补回去
/// </summary>
/// <param name="time">时间字符串</param>
/// <param name="fmt">格式化</param>
/// <returns>秒级时间戳。解析或换算失败返回 0，与 1970-01-01 00:00:00 UTC 这个合法值分不开；
///     1970 年以前的时刻：mktime 收负值的平台返回负数按 uint64 回绕后的值(恰为 -1 那一秒算失败返回 0)，
///     Windows 的 mktime 不收，返回 0</returns>
uint64_t strtots(const char *time, const char *fmt);

#endif//STIME_H_
