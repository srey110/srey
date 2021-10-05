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
* \brief          获取socket可读长度
* \param fd       socket句柄
* \return         ERR_FAILED 失败
* \return         长度
*/
int32_t socknread(const SOCKET &fd);
/*
* \brief          socket讀取數據
* \param fd       socket句柄
* \return         ERR_FAILED 失败，需要關閉socket
* \return         长度
*/
int32_t sockrecv(const SOCKET &fd, class cbuffer *pbuf);
/*
* \brief          创建一监听socket
* \param ip       ip
* \param port     port
* \param backlog  等待连接队列的最大长度 -1 使用128
* \return         INVALID_SOCK 失败
*/
SOCKET socklsn(const char *ip, const uint16_t &port, const int32_t &backlog);
/*
* \brief          创建一socket链接
* \param ip       ip
* \param port     port
* \return         INVALID_SOCK 失败
*/
SOCKET sockcnt(const char *ip, const uint16_t &port);
/*
* \brief          设置socket参数 TCP_NODELAY  SO_KEEPALIVE 非阻塞
* \param fd       SOCKET
*/
void sockopts(SOCKET &fd);
/*
* \brief          一组相互链接的socket
* \param sock     SOCKET
* \return         true 成功
*/
bool sockpair(SOCKET sock[2]);
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
* \brief          siphash
* \param pin      待计算
* \param inlen    pin长度
* \param seed0    seed
* \param seed1    seed
* \return         siphash值
*/
uint64_t siphash64(const uint8_t *pin, const size_t &inlen, 
    const uint64_t &seed0, const uint64_t &seed1);
/*
* \brief          murmur hash3
* \param key      待计算
* \param len      key长度
* \param seed     seed
* \return         murmur hash值
*/
uint64_t murmurhash3(const void *key, const size_t &len, const uint32_t &seed);
/*
* \brief          格式化字符串
* \param pformat  格式
* \param args     变参
* \return         格式化后的字符串
*/
std::string formatv(const char *pformat, va_list args);
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
std::vector<std::string> split(const std::string &str, const char *pflag, const bool empty = true);

SREY_NS_END

#endif//UTILS_H_
