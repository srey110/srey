#ifndef MYSQL_PACK_H_
#define MYSQL_PACK_H_

#include "protocol/mysql/mysql_bind.h"

//ÇëÇó°ü
void *mysql_pack_quit(mysql_ctx *mysql, size_t *size);
void *mysql_pack_selectdb(mysql_ctx *mysql, const char *database, size_t *size);
void *mysql_pack_ping(mysql_ctx *mysql, size_t *size);
void *mysql_pack_query(mysql_ctx *mysql, const char *sql, mysql_bind_ctx *mbind, size_t *size);
//stmt
void *mysql_pack_stmt_prepare(mysql_ctx *mysql, const char *sql, size_t *size);
void *mysql_pack_stmt_execute(mysql_stmt_ctx *stmt, mysql_bind_ctx *mbind, size_t *size);
void *mysql_pack_stmt_reset(mysql_stmt_ctx *stmt, size_t *size);
void *mysql_pack_stmt_close(mysql_stmt_ctx *stmt, size_t *size);

#endif//MYSQL_PACK_H_
