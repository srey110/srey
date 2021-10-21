#ifndef UTILS_H_
#define UTILS_H_

#include "macro.h"

/*
* \brief          uint64_t字节序转换
* \param ulval    需要转换的值
* \return         转换后的值
*/
uint64_t ntohl64(const uint64_t ulval);
/*
* \brief          cpu核数
* \return         cpu核数
*/
uint16_t procsnum();
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
void timeofday(struct timeval *ptv);
/*
* \brief          获取当前时间戳  毫秒
* \return         时间  毫秒
*/
uint64_t nowmsec();
/*
* \brief          获取当前时间戳  秒
* \return         时间  秒
*/
uint64_t nowsec();
/*
* \brief          格式化输出当前时间戳 秒
* \param pformat  格式   %Y-%m-%d %H:%M:%S 
* \param atime    格式后的时间字符串  秒       
*/ 
void nowtime(const char *pformat, char atime[TIME_LENS]);
/*
* \brief          格式化输出当前时间戳 毫秒
* \param pformat  格式   %Y-%m-%d %H:%M:%S
* \param atime    格式后的时间字符串 毫秒
*/
void nowmtime(const char *pformat, char atime[TIME_LENS]);
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
