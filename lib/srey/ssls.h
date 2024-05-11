#ifndef SREY_SSLS_H_
#define SREY_SSLS_H_

#include "srey/spub.h"

#if WITH_SSL

int32_t srey_ssl_register(scheduler_ctx *scheduler, name_t name, struct evssl_ctx *evssl);
struct evssl_ctx *srey_ssl_qury(scheduler_ctx *scheduler, name_t name);

#endif
#endif//SREY_SSLS_H_
