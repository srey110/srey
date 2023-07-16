#ifndef DNS_H_
#define DNS_H_

#include "structs.h"

typedef struct dns_ip {
    char ip[IP_LENS];
}dns_ip;

//buf±ØĞëÖÃ0
size_t dns_request_pack(char *buf, const char *domain, int32_t ipv6);
//ĞèÒªFREE
dns_ip *dns_parse_pack(char *buf, size_t *cnt);

#endif//DNS_H_
