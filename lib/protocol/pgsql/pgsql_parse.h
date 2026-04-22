#ifndef PGSQL_PARSE_H_
#define PGSQL_PARSE_H_

#include "protocol/pgsql/pgsql_struct.h"
#include "utils/binary.h"

char *_pgpack_error_notice(binary_ctx *breader);//ErrorResponse NoticeResponse
void _pgpack_free(pgpack_ctx *pgpack);
void _pgpack_reader_free(void *arg);
pgpack_ctx *_pgpack_parser(pgsql_ctx *pg, binary_ctx *breader, ud_cxt *ud, int32_t *status);

#endif//PGSQL_PARSE_H_
