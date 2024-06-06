#ifndef CORO_UTILS_H_
#define CORO_UTILS_H_

#include "srey/spub.h"
#include "proto/mysql.h"

#if WITH_CORO

//dns_ip *ÐèÒªFREE
struct dns_ip *coro_dns_lookup(task_ctx *task, const char *domain, int32_t ipv6, size_t *cnt);
//ws://host:port
SOCKET coro_wbsock_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ws, uint64_t *skid, int32_t netev);
SOCKET coro_redis_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ip, uint16_t port, const char *key, uint64_t *skid, int32_t netev);

int32_t mysql_connect(task_ctx *task, mysql_ctx *mysql);
void mysql_quit(task_ctx *task, mysql_ctx *mysql);
int32_t mysql_selectdb(task_ctx *task, mysql_ctx *mysql, const char *database);
int32_t mysql_ping(task_ctx *task, mysql_ctx *mysql);
mpack_ctx *mysql_query(task_ctx *task, mysql_ctx *mysql, const char *sql, mysql_bind_ctx *bind);

#endif
#endif//CORO_UTILS_H_
