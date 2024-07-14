#ifndef DNS_H_
#define DNS_H_

#include "base/structs.h"

typedef struct dns_ip {
    char ip[IP_LENS];
}dns_ip;
/// <summary>
/// dns域名解析请求包
/// </summary>
/// <param name="buf">请求包, buf必须置0</param>
/// <param name="domain">要解析的域名</param>
/// <param name="ipv6">1 ipv6 0 ipv4</param>
/// <returns>请求包长度</returns>
size_t dns_request_pack(char *buf, const char *domain, int32_t ipv6);
/// <summary>
/// 解析dns返回数据
/// </summary>
/// <param name="buf">dns 返回数据包</param>
/// <param name="cnt">长度</param>
/// <returns>dns_ip 需要FREE</returns>
dns_ip *dns_parse_pack(char *buf, size_t *cnt);
/// <summary>
/// 设置dns服务器IP
/// </summary>
/// <param name="ip">ip</param>
void dns_set_ip(const char *ip);
/// <summary>
/// 获取设置的dns服务器IP
/// </summary>
/// <returns>ip</returns>
const char *dns_get_ip(void);

#endif//DNS_H_
