#ifndef PGSQL_PARSE_H_
#define PGSQL_PARSE_H_

#include "protocol/pgsql/pgsql_struct.h"
#include "utils/binary.h"

char *_pgpack_error(pgsql_ctx *pg, binary_ctx *breader);//ErrorResponse 
char *_pgpack_notice(pgsql_ctx *pg, binary_ctx *breader);//NoticeResponse
pgpack_ctx *_pgsql_parser(pgsql_ctx *pg, binary_ctx *breader, ud_cxt *ud, int32_t *status);

#endif//PGSQL_PARSE_H_
