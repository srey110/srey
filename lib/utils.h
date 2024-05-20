#ifndef UTILS_H_
#define UTILS_H_

#include "macro.h"

typedef void *(*chr_func)(const void *, int32_t, size_t);
typedef int32_t(*cmp_func)(const void *, const void *, size_t);

//获取一ID
uint64_t createid(void);
//当前线程Id
uint64_t threadid(void);
//启coredump socket链接数限制
void unlimit(void);
//信号处理
void sighandle(void(*cb)(int32_t, void *), void *data);
//cpu核心数
uint32_t procscnt(void);

//是否为文件
int32_t isfile(const char *file);
//是否为文件夹
int32_t isdir(const char *path);
//文件大小 ERR_FAILED 失败
int64_t filesize(const char *file);
//当前程序所在路径
const char *procpath(void);

//timeofday
void timeofday(struct timeval *tv);
//与UTC时差 分钟
int32_t timeoffset(void);
//当前时间戳  毫秒
uint64_t nowms(void);
//当前时间戳  秒
uint64_t nowsec(void);
//格式化输出时间戳 秒   %Y-%m-%d %H:%M:%S 
void sectostr(uint64_t sec, const char *fmt, char time[TIME_LENS]);
//格式化输出时间戳 毫秒  %Y-%m-%d %H:%M:%S
void mstostr(uint64_t ms, const char *fmt, char time[TIME_LENS]);
//字符串转时间戳 %Y-%m-%d %H:%M:%S 
uint64_t strtots(const char *time, const char *fmt);

void fill_timespec(struct timespec* timeout, uint32_t ms);

uint64_t hash(const char *buf, size_t len);

void *memichr(const void *ptr, int32_t val, size_t maxlen);
//内存查找 ncs 0 区分大小写
void *memstr(int32_t ncs, const void *ptr, size_t plens, const void *what, size_t wlen);
//跳过空字节
void *skipempty(const void *ptr, size_t plens);
//转大写
char *strupper(char *str);
//转小写
char *strlower(char *str);
//反转
char* strreverse(char* str);
//随机[min, max)
int32_t randrange(int32_t min, int32_t max);
//随机字符串
char *randstr(char *buf, size_t len);
//转16进制字符串 out 长度为 HEX_ENSIZE
#define HEX_ENSIZE(s) (s * 2 + 1)
char *tohex(const unsigned char *buf, size_t len, char *out);
//返回值需要free
struct buf_ctx *split(const void *ptr, size_t plens, const void *sep, size_t seplens, size_t *n);
//变参 返回值需要free
char *formatargs(const char *fmt, va_list args);
char *formatv(const char *fmt, ...);

#endif//UTILS_H_
