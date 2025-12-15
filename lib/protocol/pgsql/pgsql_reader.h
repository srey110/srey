#ifndef PGSQL_READER_H_
#define PGSQL_READER_H_

#include "protocol/pgsql/pgsql_struct.h"

pgsql_reader_ctx *pgsql_reader_init(pgpack_ctx *pgpack, int16_t format);
void pgsql_reader_free(pgsql_reader_ctx *reader);
size_t pgsql_reader_size(pgsql_reader_ctx *reader);
void pgsql_reader_seek(pgsql_reader_ctx *reader, size_t pos);
int32_t pgsql_reader_eof(pgsql_reader_ctx *reader);
void pgsql_reader_next(pgsql_reader_ctx *reader);
pgpack_row *pgsql_reader_read(pgsql_reader_ctx *reader, const char *name, pgpack_field **field);

#endif//PGSQL_READER_H_
