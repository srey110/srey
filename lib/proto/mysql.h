#ifndef MYSQL_H_
#define MYSQL_H_

#include "proto/mysql_struct.h"
#include "event/event.h"
#include "srey/spub.h"

void _mysql_pkfree(void *pack);
void _mysql_init(void *hspush);
void _mysql_udfree(ud_cxt *ud);
int32_t _mysql_ssl_exchanged(ev_ctx *ev, ud_cxt *ud);

void *mysql_unpack(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status);

int32_t mysql_init(mysql_ctx *mysql, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, const char *database, const char *charset, uint32_t maxpk);
int32_t mysql_try_connect(task_ctx *task, mysql_ctx *mysql);

//closes the connection or returns ERR_Packet
void *mysql_pack_quit(mysql_ctx *mysql, size_t *size);
//OK_Packet on success ERR_Packet on error
void *mysql_pack_selectdb(mysql_ctx *mysql, const char *database, size_t *size);
//OK_Packet
void *mysql_pack_ping(mysql_ctx *mysql, size_t *size);
//ERR_Packet OK_Packet LOCAL INFILE Request Text Resultset
void *mysql_pack_query(mysql_ctx *mysql, const char *sql, size_t *size);

#endif//MYSQL_H_
