#ifndef DNS_H_
#define DNS_H_

#include "base/structs.h"
#include "utils/buffer.h"

typedef struct dns_ip {
    char ip[IP_LENS];
}dns_ip;
/// <summary>
/// dns域名解析请求包
/// </summary>
/// <param name="buf">请求包，须预留 sizeof(dns_head)+域名+sizeof(dns_question) 字节，无需调用方置零</param>
/// <param name="domain">要解析的域名</param>
/// <param name="ipv6">1 ipv6 0 ipv4</param>
/// <param name="id">输出：本次查询的随机事务 ID（主机序），供 dns_parse_pack 回验响应</param>
/// <returns>请求包长度</returns>
size_t dns_request_pack(char *buf, const char *domain, int32_t ipv6, uint16_t *id);
/// <summary>
/// dns 域名解析请求包（TCP 传输，前置 2 字节大端长度，RFC 1035 §4.2.2）
/// </summary>
/// <param name="buf">请求包，须预留 2+sizeof(dns_head)+域名+sizeof(dns_question) 字节，无需调用方置零</param>
/// <param name="domain">要解析的域名</param>
/// <param name="ipv6">1 ipv6 0 ipv4</param>
/// <param name="id">输出：本次查询的随机事务 ID（主机序），供 dns_parse_pack 回验响应</param>
/// <returns>含 2 字节长度前缀的请求包总长度，0 表示失败</returns>
size_t dns_request_pack_tcp(char *buf, const char *domain, int32_t ipv6, uint16_t *id);
/// <summary>
/// PACK_DNS 协议解包：按 2 字节大端长度切分 TCP 流，返回去除长度前缀后的 DNS 报文
/// </summary>
/// <param name="buf">接收缓冲区</param>
/// <param name="size">输出：DNS 报文长度（不含 2 字节前缀）</param>
/// <param name="status">输出：PROT_MOREDATA / PROT_ERROR</param>
/// <returns>DNS 报文指针，调用方 FREE；NULL 表示数据不足或出错</returns>
void *dns_unpack(buffer_ctx *buf, size_t *size, int32_t *status);
/// <summary>
/// 解析dns返回数据
/// </summary>
/// <param name="buf">dns 返回数据包</param>
/// <param name="buflen">数据包长度</param>
/// <param name="cnt">解析到的 IP 数量</param>
/// <param name="id">期望的事务 ID（dns_request_pack 输出）；响应事务 ID 不匹配即视为错配/伪造返回 NULL</param>
/// <returns>dns_ip 需要FREE；事务 ID 不匹配、响应被截断(TC 位置位)或解析失败均返回 NULL</returns>
dns_ip *dns_parse_pack(char *buf, size_t buflen, size_t *cnt, uint16_t id);
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
