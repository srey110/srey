#ifndef UTILS_H_
#define UTILS_H_

#include "base/macro.h"

typedef void *(*chr_func)(const void *, int32_t, size_t);
typedef int32_t(*cmp_func)(const void *, const void *, size_t);
/// <summary>
/// 获取一自增ID
/// </summary>
/// <returns>ID</returns>
uint64_t createid(void);
/// <summary>
/// 当前线程ID
/// </summary>
/// <returns>线程ID</returns>
uint64_t threadid(void);
/// <summary>
/// 启coredump socket链接数限制
/// </summary>
void unlimit(void);
/// <summary>
/// 信号处理
/// </summary>
/// <param name="cb">处理函数</param>
/// <param name="data">参数</param>
void sighandle(void(*cb)(int32_t, void *), void *data);
/// <summary>
/// cpu核心数
/// </summary>
/// <returns>核心数</returns>
uint32_t procscnt(void);
/// <summary>
/// 获取Content-Type
/// </summary>
/// <param name="extension">扩展名字.xx</param>
/// <returns>Content-Type</returns>
const char *contenttype(const char *extension);
/// <summary>
/// 是否为文件
/// </summary>
/// <param name="file">路径</param>
/// <returns>ERR_OK 文件</returns>
int32_t isfile(const char *file);
/// <summary>
/// 是否为文件夹
/// </summary>
/// <param name="file">路径</param>
/// <returns>ERR_OK 文件</returns>
int32_t isdir(const char *path);
/// <summary>
/// 文件大小
/// </summary>
/// <param name="file">路径</param>
/// <returns>文件大小, ERR_FAILED 失败</returns>
int64_t filesize(const char *file);
/// <summary>
/// 当前程序所在路径
/// </summary>
/// <returns>路径</returns>
const char *procpath(void);
/// <summary>
/// 读取文件全部
/// </summary>
/// <param name="file">路径</param>
/// <param name="lens">文件大小</param>
/// <returns>文件内容</returns>
char *readall(const char *file, size_t *lens);
/// <summary>
/// timeofday
/// </summary>
/// <param name="tv">timeval</param>
void timeofday(struct timeval *tv);
/// <summary>
/// 与UTC时差
/// </summary>
/// <returns>分</returns>
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
/// 格式化输出时间戳
/// </summary>
/// <param name="sec">秒</param>
/// <param name="fmt">格式化 %Y-%m-%d %H:%M:%S</param>
/// <param name="time">时间字符串</param>
void sectostr(uint64_t sec, const char *fmt, char time[TIME_LENS]);
/// <summary>
/// 格式化输出时间戳
/// </summary>
/// <param name="ms">毫秒</param>
/// <param name="fmt">格式化 %Y-%m-%d %H:%M:%S</param>
/// <param name="time">时间字符串</param>
void mstostr(uint64_t ms, const char *fmt, char time[TIME_LENS]);
/// <summary>
/// 字符串转时间戳
/// </summary>
/// <param name="time">时间字符串</param>
/// <param name="fmt">格式化</param>
/// <returns>时间戳</returns>
uint64_t strtots(const char *time, const char *fmt);
/// <summary>
/// 填充timespec
/// </summary>
/// <param name="timeout">struct timespec</param>
/// <param name="ms">毫秒</param>
void fill_timespec(struct timespec *timeout, uint32_t ms);
/// <summary>
/// hash
/// </summary>
/// <param name="buf">要计算的数据</param>
/// <param name="len">数据长度</param>
/// <returns>hash</returns>
uint64_t hash(const char *buf, size_t len);
/// <summary>
/// 查找字符，不区分大小写
/// </summary>
/// <param name="ptr">源字符</param>
/// <param name="val">需要查找的字符</param>
/// <param name="maxlen">最多搜索长度</param>
/// <returns>void * 字符出现的指针, NULL无</returns>
void *memichr(const void *ptr, int32_t val, size_t maxlen);
/// <summary>
/// 内存查找
/// </summary>
/// <param name="ncs">0 区分大小写</param>
/// <param name="ptr">源字符</param>
/// <param name="plens">源字符长度</param>
/// <param name="what">要查找的字符串</param>
/// <param name="wlen">what长度</param>
/// <returns>void * 字符出现的指针, NULL无</returns>
void *memstr(int32_t ncs, const void *ptr, size_t plens, const void *what, size_t wlen);
/// <summary>
/// 跳过空字节
/// </summary>
/// <param name="ptr">源字符</param>
/// <param name="plens">源字符长度</param>
/// <returns>void *, NULL全为空</returns>
void *skipempty(const void *ptr, size_t plens);
/// <summary>
/// 转大写
/// </summary>
/// <param name="str">源字符</param>
/// <returns>char *</returns>
char *strupper(char *str);
/// <summary>
/// 转小写
/// </summary>
/// <param name="str">源字符</param>
/// <returns>char *</returns>
char *strlower(char *str);
/// <summary>
/// 反转
/// </summary>
/// <param name="str">源字符</param>
/// <returns>char *</returns>
char* strreverse(char* str);
/// <summary>
/// 随机[min, max]
/// </summary>
/// <param name="min">最小</param>
/// <param name="max">最大</param>
/// <returns>值</returns>
int32_t randrange(int32_t min, int32_t max);
/// <summary>
/// 随机字符串
/// </summary>
/// <param name="buf">buffer</param>
/// <param name="len">随机长度</param>
/// <returns>char *</returns>
char *randstr(char *buf, size_t len);
#define HEX_ENSIZE(s) (s * 2 + 1)
/// <summary>
/// 转16进制
/// </summary>
/// <param name="buf">要转的数据</param>
/// <param name="len">数据长度</param>
/// <param name="out">转换后的数据,长度:HEX_ENSIZE</param>
/// <returns>char *</returns>
char *tohex(const void *buf, size_t len, char *out);
/// <summary>
/// 拆分
/// </summary>
/// <param name="ptr">要拆分的数据</param>
/// <param name="plens">数据长度</param>
/// <param name="sep">拆分标记</param>
/// <param name="seplens">拆分标记长度</param>
/// <param name="n">拆分后的长度</param>
/// <returns>buf_ctx *, 需要free</returns>
struct buf_ctx *split(const void *ptr, size_t plens, const void *sep, size_t seplens, size_t *n);
/// <summary>
/// 变参
/// </summary>
/// <param name="fmt">格式化</param>
/// <param name="args">变参</param>
/// <returns>char * 需要free</returns>
char *_format_va(const char *fmt, va_list args);
/// <summary>
/// 变参
/// </summary>
/// <param name="fmt">格式化</param>
/// <param name="...">变参</param>
/// <returns>char * 需要free</returns>
char *format_va(const char *fmt, ...);
/// <summary>
/// 大小端判断
/// </summary>
/// <returns>1 小端, 0 大端</returns>
int32_t is_little(void);
/// <summary>
/// 数字转 char*
/// </summary>
/// <param name="buf">buffer</param>
/// <param name="val">数字</param>
/// <param name="size">字节数</param>
/// <param name="islittle">是否为小端</param>
void pack_integer(char *buf, uint64_t val, int32_t size, int32_t islittle);
/// <summary>
/// char* 转数字
/// </summary>
/// <param name="buf">要转换的buffer</param>
/// <param name="size">字节数</param>
/// <param name="islittle">是否为小端</param>
/// <param name="issigned">是否有符号</param>
/// <returns>数字</returns>
int64_t unpack_integer(const char *buf, int32_t size, int32_t islittle, int32_t issigned);
/// <summary>
/// float转 char*
/// </summary>
/// <param name="buf">buffer</param>
/// <param name="val">值</param>
/// <param name="islittle">是否为小端</param>
void pack_float(char *buf, float val, int32_t islittle);
/// <summary>
/// char* 转float
/// </summary>
/// <param name="buf">要转换的buffer</param>
/// <param name="islittle">是否为小端</param>
/// <returns>float</returns>
float unpack_float(const char *buf, int32_t islittle);
/// <summary>
/// double转 char*
/// </summary>
/// <param name="buf">buffer</param>
/// <param name="val">值</param>
/// <param name="islittle">是否为小端</param>
void pack_double(char *buf, double val, int32_t islittle);
/// <summary>
/// char* 转double
/// </summary>
/// <param name="buf">要转换的buffer</param>
/// <param name="islittle">是否为小端</param>
/// <returns>double</returns>
double unpack_double(const char *buf, int32_t islittle);
#if !defined(OS_WIN) && !defined(OS_DARWIN) && !defined(OS_AIX)
uint64_t ntohll(uint64_t val);
uint64_t htonll(uint64_t val);
#endif

#endif//UTILS_H_
