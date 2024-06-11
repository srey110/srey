#ifndef MYSQL_BIND_H_
#define MYSQL_BIND_H_

#include "protocol/mysql/mysql_struct.h"

void mysql_bind_init(mysql_bind_ctx *mbind);
void mysql_bind_free(mysql_bind_ctx *mbind);
void mysql_bind_clear(mysql_bind_ctx *mbind);
//MYSQL_TYPE_NULL
void mysql_bind_nil(mysql_bind_ctx *mbind, const char *name);
//MYSQL_TYPE_STRING MYSQL_TYPE_VARCHAR MYSQL_TYPE_VAR_STRING MYSQL_TYPE_ENUM MYSQL_TYPE_SET
//MYSQL_TYPE_LONG_BLOB MYSQL_TYPE_MEDIUM_BLOB MYSQL_TYPE_BLOB MYSQL_TYPE_TINY_BLOB
//MYSQL_TYPE_GEOMETRY MYSQL_TYPE_BIT MYSQL_TYPE_DECIMAL MYSQL_TYPE_NEWDECIMAL MYSQL_TYPE_JSON
void mysql_bind_string(mysql_bind_ctx *mbind, const char *name, char *value, size_t lens);
//MYSQL_TYPE_TINY MYSQL_TYPE_SHORT MYSQL_TYPE_YEAR MYSQL_TYPE_LONG MYSQL_TYPE_LONGLONG
void mysql_bind_integer(mysql_bind_ctx *mbind, const char *name, int64_t value);
void mysql_bind_uinteger(mysql_bind_ctx *mbind, const char *name, uint64_t value);
void mysql_bind_double(mysql_bind_ctx *mbind, const char *name, double value);
//MYSQL_TYPE_DATE MYSQL_TYPE_DATETIME MYSQL_TYPE_TIMESTAMP
void mysql_bind_datetime(mysql_bind_ctx *mbind, const char *name, time_t ts);
//MYSQL_TYPE_TIME
void mysql_bind_time(mysql_bind_ctx *mbind, const char *name,
    int8_t is_negative, int32_t days, int8_t hour, int8_t minute, int8_t second);

#endif//MYSQL_BIND_H_
