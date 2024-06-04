#ifndef MYSQL_PARSE_H_
#define MYSQL_PARSE_H_

#include "proto/mysql_struct.h"
#include "binary.h"

void _mysql_set_lenenc_int(binary_ctx *breader, size_t integer);
int32_t _mpack_ok(binary_ctx *breader, mpack_ok *ok);
int32_t _mpack_err(binary_ctx *breader, mpack_err *err);
int32_t _mpack_parser(mysql_ctx *mysql, binary_ctx *breader, mpack_ctx *mpack);

#endif//MYSQL_PARSE_H_
