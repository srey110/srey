#ifndef PGSQL_PACK_H_
#define PGSQL_PACK_H_

#include "protocol/pgsql/pgsql_struct.h"

void *pgsql_pack_quit(pgsql_ctx *pg, size_t *size);
void *pgsql_pack_query(pgsql_ctx *pg, const char *sql, size_t *size);

#endif//PGSQL_PACK_H_
