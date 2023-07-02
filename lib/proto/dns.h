#ifndef DNS_H_
#define DNS_H_

#include "structs.h"

typedef struct dns_ip {
    char ip[IP_LENS];
}dns_ip;

dns_ip *dns_lookup(struct task_ctx *task, const char *dns, const char *domain, int32_t ipv6, size_t *cnt);

#endif//DNS_H_
