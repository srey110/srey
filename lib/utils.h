#ifndef UTILS_H_
#define UTILS_H_

#include "macro.h"

SREY_NS_BEGIN
/*
* \brief          uint64_t字节序转换
* \param ulval    需要转换的值
* \return         转换后的值
*/
uint64_t ntohl64(const uint64_t &ulval);
/*
* \brief          线程Id
* \return         线程Id
*/
uint32_t threadid();
/*
* \brief          cpu核数
* \return         cpu核数
*/
uint32_t procsnum();
/*
* \brief          判断文件是否存在
* \param pname    文件名
* \return         true 存在
*/
bool fileexist(const char *pname);
/*
* \brief          判断是否为文件
* \param pname    文件名
* \return         true 是
*/
bool isfile(const char *pname);
/*
* \brief          判断是否为文件夹
* \param pname    文件名
* \return         true 是
*/
bool isdir(const char *pname);
/*
* \brief          文件大小
* \param pname    文件名
* \return         ERR_FAILED 失败
* \return         文件大小
*/
int64_t filesize(const char *pname);
/*
* \brief          文件路径
* \param path     全路径
* \return         路径
*/
std::string dirnam(const char *path);
/*
* \brief          获取当前程序所在路径
* \return         ""失败
* \return         路径
*/
std::string getpath();
/*
* \brief          获取路径下所有文件
* \param ppath    路径
* \param lstname  结果
* \param bdir     true 文件夹
* \return         文件/文件夹
*/
void filefind(const char *ppath ,std::list<std::string> &lstname, const bool bdir = false);
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
* \brief          固定格式输出当前时间戳 毫秒
* \param atime    当前时间 毫秒
*/
std::string nowmtime();
/*
* \brief          计算crc16
* \param pval     待计算
* \param ilen     pval长度
* \return         crc16值
*/
uint16_t crc16(const char *pval, const size_t &ilen);
/*
* \brief          计算crc32
* \param pval     待计算
* \param ilen     pval长度
* \return         crc32值
*/
uint32_t crc32(const char *pval, const size_t &ilen);
/*
* \brief          计算md5
* \param pval     待计算
* \param ilens    pval长度
* \param md5str   md5值
*/
void md5(const char *pval, const size_t &ilens, char md5str[33]);
/*
* \brief          计算sha1
* \param pval     待计算
* \param ilens    pval长度
* \param md5str   sha1值
*/
void sha1(const char *pval, const size_t &ilens, char md5str[20]);

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
int32_t b64encode(const char *pval, const size_t &ilens, char *pout);
/*
* \brief          base64解码
* \param pval     待转换的
* \param ilens    pval长度
* \return         ERR_FAILED 失败 
* \return         解码长度
*/
int32_t b64decode(const char *pval, const size_t &ilens, char *pout);
/*
* \brief          字符串转大写
* \param pval     待转换的字符串
* \return         转换化后的字符串
*/
char *toupper(char *pval);
/*
* \brief          字符串转小写
* \param pval     待转换的字符串
* \return         转换化后的字符串
*/
char *tolower(char *pval);
/*
* \brief          转16进制字符串
* \param pval     待转换的
* \param ilens    pval长度 
* \param bspace   是否以空格分开
* \return         转换化后的字符串
*/
std::string tohex(const char *pval, const size_t &ilens, const bool bspace = true);
/*
* \brief          格式化字符串
* \param pformat  格式
* \param args     变参
* \param iinit    初始化时内存大小
* \return         格式化后的字符串,需要delete
*/
char *formatv(const char *pformat, va_list args, const size_t &iinit = 128);
/*
* \brief          格式化字符串
* \param pformat  格式
* \param iinit    初始化时内存大小
* \param args     变参
* \return         格式化后的字符串,需要delete
*/
char *formatv(const char *pformat, ...);
/*
* \brief          格式化字符串
* \param pformat  格式
* \param ...      变参
* \return         格式化后的字符串
*/
std::string formatstr(const char *pformat, ...);
/*
* \brief          移除左边特殊字符串
* \param str      待拆字符串
* \return         处理后的数据
*/
std::string triml(const std::string &str);
/*
* \brief          移除右边特殊字符串
* \param str      待拆字符串
* \return         处理后的数据
*/
std::string trimr(const std::string &str);
/*
* \brief          移除两边特殊字符串
* \param str      待拆字符串
* \return         处理后的数据
*/
std::string trim(const std::string &str);
/*
* \brief          拆分字符串
* \param str      待拆字符串
* \param pflag    拆分标志
* \param empty    是否包含空字符串
* \return         拆分后的数据，不包含空字符串
*/
void split(const std::string &str, const char *pflag, std::vector<std::string> &tokens, const bool empty = true);

SREY_NS_END

#endif//UTILS_H_
