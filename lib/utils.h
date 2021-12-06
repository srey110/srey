#ifndef UTILS_H_
#define UTILS_H_

#include "macro.h"

/*
* \brief          开启coredump socket链接数限制
*/
void unlimit();
/*
* \brief          信号处理
*/
void sighandle(void(*sig_cb)(int32_t, void *), void *pud);
/*
* \brief          判断系统是否为大端
* \param ulval    需要转换的值
* \return         转换后的值
*/
int32_t bigendian();
/*
* \brief          cpu核数
* \return         cpu核数
*/
uint32_t procscnt();
/*
* \brief          判断是否为文件
* \param pname    文件名
* \return         ERR_OK 是
*/
int32_t isfile(const char *pname);
/*
* \brief          判断是否为文件夹
* \param pname    文件名
* \return         ERR_OK 是
*/
int32_t isdir(const char *pname);
/*
* \brief          文件大小
* \param pname    文件名
* \return         ERR_FAILED 失败
* \return         文件大小
*/
int64_t filesize(const char *pname);
/*
* \brief          获取当前程序所在路径
* \param acpath   路径
* \return         ERR_OK 成功
*/
int32_t getprocpath(char acpath[PATH_LENS]);
/*
* \brief          获取时间
* \param ptv      timeval
*/
static inline void timeofday(struct timeval *ptv)
{
#if defined(OS_WIN)
    #define U64_LITERAL(n) n##ui64
    #define EPOCH_BIAS U64_LITERAL(116444736000000000)
    #define UNITS_PER_SEC U64_LITERAL(10000000)
    #define USEC_PER_SEC U64_LITERAL(1000000)
    #define UNITS_PER_USEC U64_LITERAL(10)
    union
    {
        FILETIME ft_ft;
        uint64_t ft_64;
    } ft;

    GetSystemTimeAsFileTime(&ft.ft_ft);
    ft.ft_64 -= EPOCH_BIAS;
    ptv->tv_sec = (long)(ft.ft_64 / UNITS_PER_SEC);
    ptv->tv_usec = (long)((ft.ft_64 / UNITS_PER_USEC) % USEC_PER_SEC);
#else
    (void)gettimeofday(ptv, NULL);
#endif
};
/*
* \brief          获取当前时间戳  毫秒
* \return         时间  毫秒
*/
static inline uint64_t nowmsec()
{
    struct timeval tv;
    timeofday(&tv);
    return (uint64_t)tv.tv_usec / 1000 + (uint64_t)tv.tv_sec * 1000;
};
/*
* \brief          获取当前时间戳  秒
* \return         时间  秒
*/
static inline uint64_t nowsec()
{
    struct timeval tv;
    timeofday(&tv);
    return (uint64_t)tv.tv_sec;
};
/*
* \brief          格式化输出当前时间戳 秒
* \param pformat  格式   %Y-%m-%d %H:%M:%S 
* \param atime    格式后的时间字符串  秒       
*/ 
static inline void nowtime(const char *pformat, char atime[TIME_LENS])
{
    struct timeval tv;
    timeofday(&tv);
    time_t t = tv.tv_sec;
    ZERO(atime, TIME_LENS);
    strftime(atime, TIME_LENS - 1, pformat, localtime(&t));
};
/*
* \brief          格式化输出当前时间戳 毫秒
* \param pformat  格式   %Y-%m-%d %H:%M:%S
* \param atime    格式后的时间字符串 毫秒
*/
static inline void nowmtime(const char *pformat, char atime[TIME_LENS])
{
    struct timeval tv;
    timeofday(&tv);
    time_t t = tv.tv_sec;
    ZERO(atime, TIME_LENS);
    strftime(atime, TIME_LENS - 1, pformat, localtime(&t));
    size_t uilen = strlen(atime);
    SNPRINTF(atime + uilen, TIME_LENS - uilen - 1, " %03d", (int32_t)(tv.tv_usec / 1000));
};
/*
* \brief          计算crc16
* \param pval     待计算
* \param ilen     pval长度
* \return         crc16值
*/
uint16_t crc16(const char *pval, const size_t ilen);
/*
* \brief          计算crc32
* \param pval     待计算
* \param ilen     pval长度
* \return         crc32值
*/
uint32_t crc32(const char *pval, const size_t ilen);
/*
* \brief          计算md5
* \param pval     待计算
* \param ilens    pval长度
* \param md5str   md5值
*/
void md5(const char *pval, const size_t ilens, char md5str[33]);
/*
* \brief          计算sha1
* \param pval     待计算
* \param ilens    pval长度
* \param md5str   sha1值
*/
void sha1(const char *pval, const size_t ilens, char md5str[20]);

//长度
#define B64_ENSIZE(s)   (((s) + 2) / 3 * 4)
#define B64_DESIZE(s)   (((s)) / 4 * 3)
/*
* \brief          转base64
* \param pval     待转换的
* \param ilens    pval长度
* \return         ERR_FAILED 失败
* \return         编码长度
*/
int32_t b64encode(const char *pval, const size_t ilens, char *pout);
/*
* \brief          base64解码
* \param pval     待转换的
* \param ilens    pval长度
* \return         ERR_FAILED 失败 
* \return         解码长度
*/
int32_t b64decode(const char *pval, const size_t ilens, char *pout);
char *xorencode(const char ackey[4], const size_t uiround, char *pbuf, const size_t uilens);
char *xordecode(const char ackey[4], const size_t uiround, char *pbuf, const size_t uilens);
/*
* \brief          hash
* \return         hash
*/
uint64_t hash(const char *pfirst, size_t uilen);
uint64_t fnv1a_hash(const char *pfirst, size_t uilen);
/*
* \brief          字符串转大写
* \param pval     待转换的字符串
* \return         转换化后的字符串
*/
char *strtoupper(char *pval);
/*
* \brief          字符串转小写
* \param pval     待转换的字符串
* \return         转换化后的字符串
*/
char *strtolower(char *pval);
/*
* \brief          转16进制字符串
* \param pval     待转换的
* \param ilens    pval长度 
* \param          转换化后的字符串  长度为 ilens * 3 + 1
*/
void tohex(const char *pval, const size_t ilens, char *pout);
/*
* \brief          格式化字符串
* \param pformat  格式
* \param args     变参
* \param iinit    初始化时内存大小
* \return         格式化后的字符串,需要delete
*/
char *formatargs(const char *pformat, va_list args);
/*
* \brief          格式化字符串
* \param pformat  格式
* \param iinit    初始化时内存大小
* \param args     变参
* \return         格式化后的字符串,需要delete
*/
char *formatv(const char *pformat, ...);

#endif//UTILS_H_
