#ifndef MYSQL_PARSE_H_
#define MYSQL_PARSE_H_

#include "protocol/mysql/mysql_struct.h"
#include "utils/buffer.h"

/// <summary>
/// stmt ≥ı ºªØ
/// </summary>
/// <param name="mpack">mpack_ctx</param>
/// <returns>mysql_stmt_ctx  NULL  ß∞‹</returns>
mysql_stmt_ctx *mysql_stmt_init(mpack_ctx *mpack);
void _mpack_stm_free(void *pack);
void _mpack_reader_free(void *pack);
char *_mysql_payload(mysql_ctx *mysql, buffer_ctx *buf, size_t *payload_lens, int32_t *status);

void _mpack_ok(mysql_ctx *mysql, binary_ctx *breader, mpack_ok *ok);
void _mpack_err(mysql_ctx *mysql, binary_ctx *breader, mpack_err *err);
mpack_ctx *_mpack_parser(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status);

#endif//MYSQL_PARSE_H_
