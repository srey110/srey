#include "base/stime.h"
#include "base/err.h"
#include "base/strptime.h"

#if defined(OS_WIN)
// 只有 _now_usec 用;这几条在 macOS/Linux 上从不编译, 改了没有本机回归网兜着
#define U64_LITERAL(n) n##ui64
#define EPOCH_BIAS U64_LITERAL(116444736000000000) //Windows FILETIME 纪元与 Unix 纪元的差值（100ns 单位）
#define UNITS_PER_USEC U64_LITERAL(10)//每微秒的 100ns 单位数
// FILETIME 与 uint64 共用同一块内存:GetSystemTimeAsFileTime 写前者,算术走后者
typedef union filetime_u64 {
    FILETIME ft_ft;
    uint64_t ft_64;
}filetime_u64;
#endif

// 两份 tm 取自同一个 now 直接作差。不走 mktime:那条路要另外补夏令时,而补多少、按哪个时刻判,
// 两样都取不准(有半小时制的时区)
int32_t timeoffset(void) {
    time_t now = time(NULL);
    struct tm gmt_tm, loc_tm;
    if (0 != GMTIME(&now, &gmt_tm)
        || 0 != LOCALTIME(&now, &loc_tm)) {
        return 0;
    }
    int32_t days = loc_tm.tm_yday - gmt_tm.tm_yday;
    if (loc_tm.tm_year != gmt_tm.tm_year) {
        days = loc_tm.tm_year > gmt_tm.tm_year ? 1 : -1;
    }
    return (days * 24 + loc_tm.tm_hour - gmt_tm.tm_hour) * 60
        + loc_tm.tm_min - gmt_tm.tm_min;
}
// Unix 纪元起的微秒数,全程 64 位。timeofday 反过来由它推导:Windows 的 struct timeval.tv_sec
// 是 32 位 long(LLP64),2038-01-19 后回绕为负,再转 uint64 会符号扩展成约 1.8e19
static uint64_t _now_usec(void) {
#if defined(OS_WIN)
    filetime_u64 ft;
    GetSystemTimeAsFileTime(&ft.ft_ft);
    ft.ft_64 -= EPOCH_BIAS;
    return ft.ft_64 / UNITS_PER_USEC;
#else
    struct timeval tv;
    (void)gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
#endif
}
uint64_t nowms(void) {
    return _now_usec() / 1000;
}
uint64_t nowsec(void) {
    return _now_usec() / 1000000;
}
void timeofday(struct timeval *tv) {
    uint64_t us = _now_usec();
    tv->tv_sec = (long)(us / 1000000);
    tv->tv_usec = (long)(us % 1000000);
}
// 秒级格式化, 成功时回填写入长度供调用方接着写后缀; 两处失败都把 time 置空串
static int32_t _sectostr_lens(uint64_t sec, const char *fmt, char time[TIME_LENS], size_t *lens) {
    time_t t = (time_t)sec;
    struct tm loc_tm;
    // 失败时 loc_tm 未必被写过,交给 strftime 就是拿不定的 tm_wday/tm_mon 去索引 libc 的静态名表
    if (0 != LOCALTIME(&t, &loc_tm)) {
        time[0] = '\0';
        return ERR_FAILED;
    }
    *lens = strftime(time, TIME_LENS - 1, fmt, &loc_tm);
    if (0 == *lens) {
        time[0] = '\0';
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t sectostr(uint64_t sec, const char *fmt, char time[TIME_LENS]) {
    size_t lens;
    return _sectostr_lens(sec, fmt, time, &lens);
}
int32_t mstostr(uint64_t ms, const char *fmt, char time[TIME_LENS]) {
    size_t lens;
    if (ERR_OK != _sectostr_lens(ms / 1000, fmt, time, &lens)) {
        return ERR_FAILED;
    }
    // 装不下就整体失败,截断的时间串拿去用是静默出错
    const size_t mslens = sizeof(" 000");
    if (TIME_LENS - lens < mslens) {
        time[0] = '\0';
        return ERR_FAILED;
    }
    SNPRINTF(time + lens, TIME_LENS - lens, " %03d", (int32_t)(ms % 1000));
    return ERR_OK;
}
uint64_t strtots(const char *time, const char *fmt) {
    struct tm dttm = { 0 };
    if (NULL == _strptime(time, fmt, &dttm)) {
        return 0;
    }
    time_t ts = mktime(&dttm);
    if ((time_t)-1 == ts) {
        return 0;
    }
    return (uint64_t)ts;
}
