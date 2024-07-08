#ifndef TASK_SSL_H_
#define TASK_SSL_H_

#include "lib.h"

#if WITH_SSL
void task_ssl_start(loader_ctx *loader, name_t name, evssl_ctx *ssl, int32_t pt);
#endif

#endif//TASK_SSL_H_
