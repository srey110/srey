#ifndef MYSQL_H_
#define MYSQL_H_

#include "proto/mysql/mysql_struct.h"
#include "event/event.h"
#include "srey/spub.h"

void _mysql_pkfree(void *pack);
void _mysql_init(void *hspush);
void _mysql_udfree(ud_cxt *ud);
void _mysql_closed(ud_cxt *ud);
int32_t _mysql_ssl_exchanged(ev_ctx *ev, ud_cxt *ud);
void *mysql_unpack(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status);

int32_t mysql_init(mysql_ctx *mysql, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, const char *database, const char *charset, uint32_t maxpk, int32_t relink);
int32_t mysql_try_connect(task_ctx *task, mysql_ctx *mysql);
const char *mysql_erro(mysql_ctx *mysql, int32_t *code);
void mysql_erro_clear(mysql_ctx *mysql);
int64_t mysql_last_id(mysql_ctx *mysql);
int64_t mysql_affected_rows(mysql_ctx *mysql);
void mysql_stmt_close(task_ctx *task, mysql_stmt_ctx *stmt);

#endif//MYSQL_H_
