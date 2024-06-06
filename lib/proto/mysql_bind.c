#include "proto/mysql_bind.h"

void _mysql_set_lenenc(binary_ctx *bwriter, size_t integer) {
    if (integer <= 0xfa) {
        binary_set_uint8(bwriter, (uint8_t)integer);
        return;
    }
    if (integer <= USHRT_MAX) {
        binary_set_uint8(bwriter, 0xfc);
        binary_set_integer(bwriter, (int64_t)integer, 2, 1);
        return;
    }
    if (integer <= INT3_MAX) {
        binary_set_uint8(bwriter, 0xfd);
        binary_set_integer(bwriter, (int64_t)integer, 3, 1);
        return;
    }
    binary_set_uint8(bwriter, 0xfe);
    binary_set_integer(bwriter, (int64_t)integer, 8, 1);
}
void mysql_bind_init(mysql_bind_ctx *bind) {
    bind->count = 0;
    binary_init(&bind->bitmap, NULL, 0, 0);
    binary_init(&bind->type, NULL, 0, 0);
    binary_init(&bind->type_name, NULL, 0, 0);
    binary_init(&bind->value, NULL, 0, 0);
}
void mysql_bind_free(mysql_bind_ctx *bind) {
    FREE(bind->bitmap.data);
    FREE(bind->type.data);
    FREE(bind->type_name.data);
    FREE(bind->value.data);
}
void mysql_bind_clear(mysql_bind_ctx *bind) {
    bind->count = 0;
    binary_offset(&bind->bitmap, 0);
    binary_offset(&bind->type, 0);
    binary_offset(&bind->type_name, 0);
    binary_offset(&bind->value, 0);
}
static void _mysql_bind_bitmap(mysql_bind_ctx *bind, int32_t nil) {
    int32_t index = bind->count % 8;
    if (0 == index) {
        binary_set_fill(&bind->bitmap, 0, 1);
    }
    char *bitmap = binary_at(&bind->bitmap, bind->bitmap.offset - 1);
    if (nil) {
        bitmap[0] |= (1 << index);
    }
    ++bind->count;
}
static void _mysql_bind_type_name(mysql_bind_ctx *bind, mysql_field_types type, const char *name) {
    binary_set_integer(&bind->type, type, 2, 1);
    binary_set_integer(&bind->type_name, type, 2, 1);
    size_t lens = strlen(name);
    _mysql_set_lenenc(&bind->type_name, lens);
    binary_set_string(&bind->type_name, name, lens);
}
void mysql_bind_nil(mysql_bind_ctx *bind, const char *name) {
    _mysql_bind_bitmap(bind, 1);
    _mysql_bind_type_name(bind, MYSQL_TYPE_NULL, name);
}
void mysql_bind_string(mysql_bind_ctx *bind, const char *name, char *value, size_t lens) {
    _mysql_bind_bitmap(bind, 0);
    _mysql_bind_type_name(bind, MYSQL_TYPE_STRING, name);
    _mysql_set_lenenc(&bind->value, lens);
    binary_set_string(&bind->value, value, lens);
}
void mysql_bind_int8(mysql_bind_ctx *bind, const char *name, int8_t value) {
    _mysql_bind_bitmap(bind, 0);
    _mysql_bind_type_name(bind, MYSQL_TYPE_TINY, name);
    binary_set_int8(&bind->value, value);
}
void mysql_bind_int16(mysql_bind_ctx *bind, const char *name, int16_t value) {
    _mysql_bind_bitmap(bind, 0);
    _mysql_bind_type_name(bind, MYSQL_TYPE_SHORT, name);
    binary_set_integer(&bind->value, value, sizeof(int16_t), 1);
}
void mysql_bind_int32(mysql_bind_ctx *bind, const char *name, int32_t value) {
    _mysql_bind_bitmap(bind, 0);
    _mysql_bind_type_name(bind, MYSQL_TYPE_LONG, name);
    binary_set_integer(&bind->value, value, sizeof(int32_t), 1);
}
void mysql_bind_int64(mysql_bind_ctx *bind, const char *name, int64_t value) {
    _mysql_bind_bitmap(bind, 0);
    _mysql_bind_type_name(bind, MYSQL_TYPE_LONGLONG, name);
    binary_set_integer(&bind->value, value, sizeof(int64_t), 1);
}
void mysql_bind_float(mysql_bind_ctx *bind, const char *name, float value) {
    _mysql_bind_bitmap(bind, 0);
    _mysql_bind_type_name(bind, MYSQL_TYPE_FLOAT, name);
    binary_set_float(&bind->value, value, 1);
}
void mysql_bind_double(mysql_bind_ctx *bind, const char *name, double value) {
    _mysql_bind_bitmap(bind, 0);
    _mysql_bind_type_name(bind, MYSQL_TYPE_DOUBLE, name);
    binary_set_double(&bind->value, value, 1);
}
void mysql_bind_datetime(mysql_bind_ctx *bind, const char *name, time_t ts) {
    _mysql_bind_bitmap(bind, 0);
    _mysql_bind_type_name(bind, MYSQL_TYPE_DATETIME, name);
    struct tm *dt = localtime(&ts);
    if (0 == dt->tm_hour && 0 == dt->tm_min && 0 == dt->tm_sec) {
        binary_set_int8(&bind->value, 4);
        binary_set_integer(&bind->value, dt->tm_year + 1900, 2, 1);
        binary_set_int8(&bind->value, dt->tm_mon + 1);
        binary_set_int8(&bind->value, dt->tm_mday);
        return;
    }
    binary_set_int8(&bind->value, 7);
    binary_set_integer(&bind->value, dt->tm_year + 1900, 2, 1);
    binary_set_int8(&bind->value, dt->tm_mon + 1);
    binary_set_int8(&bind->value, dt->tm_mday);
    binary_set_int8(&bind->value, dt->tm_hour);
    binary_set_int8(&bind->value, dt->tm_min);
    binary_set_int8(&bind->value, dt->tm_sec);
}
void mysql_bind_time(mysql_bind_ctx *bind, const char *name,
    int8_t is_negative, int32_t days, int8_t hour, int8_t minute, int8_t second) {
    _mysql_bind_bitmap(bind, 0);
    _mysql_bind_type_name(bind, MYSQL_TYPE_TIME, name);
    if (0 == days && 0 == hour && 0 == minute && 0 == second) {
        binary_set_int8(&bind->value, 0);
        return;
    }
    binary_set_int8(&bind->value, 8);
    binary_set_int8(&bind->value, is_negative);
    binary_set_integer(&bind->value, days, 4, 1);
    binary_set_int8(&bind->value, hour);
    binary_set_int8(&bind->value, minute);
    binary_set_int8(&bind->value, second);
}
