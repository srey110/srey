#ifndef DNS_H_
#define DNS_H_

#include "structs.h"

struct dns_pack_ctx *dns_unpack(char *buf, size_t size);

#endif//DNS_H_
