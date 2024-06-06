#ifndef MYSQL_PARSE_H_
#define MYSQL_PARSE_H_

#include "proto/mysql_struct.h"
#include "buffer.h"

char *_mysql_payload(mysql_ctx *mysql, buffer_ctx *buf, size_t *payload_lens, int32_t *status);
void _mpack_ok(binary_ctx *breader, mpack_ok *ok);
void _mpack_err(binary_ctx *breader, mpack_err *err);
mpack_ctx *_mpack_parser(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status);

#endif//MYSQL_PARSE_H_
