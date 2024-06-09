#ifndef MYSQL_READER_H_
#define MYSQL_READER_H_

#include "proto/mysql/mysql.h"

mysql_reader_ctx *mysql_reader_init(mpack_ctx *mpack);
void mysql_reader_free(mysql_reader_ctx *reader);

size_t mysql_reader_size(mysql_reader_ctx *reader);
void mysql_reader_seek(mysql_reader_ctx *reader, size_t pos);

int32_t mysql_reader_eof(mysql_reader_ctx *reader);
void mysql_reader_next(mysql_reader_ctx *reader);
//err ERR_OK ³É¹¦  ERR_FAILED Ê§°Ü 1 nil
int64_t mysql_reader_integer(mysql_reader_ctx *reader, const char *name, int32_t *err);
uint64_t mysql_reader_uinteger(mysql_reader_ctx *reader, const char *name, int32_t *err);
double mysql_reader_double(mysql_reader_ctx *reader, const char *name, int32_t *err);
char *mysql_reader_string(mysql_reader_ctx *reader, const char *name, size_t *lens, int32_t *err);
uint64_t mysql_reader_datetime(mysql_reader_ctx *reader, const char *name, int32_t *err);
//return is_negative 1 if minus, 0 for plus
int32_t mysql_reader_time(mysql_reader_ctx *reader, const char *name, struct tm *time, int32_t *err);

#endif//MYSQL_READER_H_
