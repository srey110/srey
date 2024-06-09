#include "proto/mysql/mysql_bind.h"
#include "proto/mysql/mysql_utils.h"

void mysql_bind_init(mysql_bind_ctx *mbind) {
    mbind->count = 0;
    binary_init(&mbind->bitmap, NULL, 0, 0);
    binary_init(&mbind->type, NULL, 0, 0);
    binary_init(&mbind->type_name, NULL, 0, 0);
    binary_init(&mbind->value, NULL, 0, 0);
}
void mysql_bind_free(mysql_bind_ctx *mbind) {
    FREE(mbind->bitmap.data);
    FREE(mbind->type.data);
    FREE(mbind->type_name.data);
    FREE(mbind->value.data);
}
void mysql_bind_clear(mysql_bind_ctx *mbind) {
    mbind->count = 0;
    binary_offset(&mbind->bitmap, 0);
    binary_offset(&mbind->type, 0);
    binary_offset(&mbind->type_name, 0);
    binary_offset(&mbind->value, 0);
}
static void _mysql_bind_bitmap(mysql_bind_ctx *mbind, int32_t nil) {
    int32_t index = mbind->count % 8;
    if (0 == index) {
        binary_set_fill(&mbind->bitmap, 0, 1);
    }
    char *bitmap = binary_at(&mbind->bitmap, mbind->bitmap.offset - 1);
    if (nil) {
        bitmap[0] |= (1 << index);
    }
    ++mbind->count;
}
static void _mysql_bind_type_name(mysql_bind_ctx *mbind, mysql_field_types type, const char *name, int32_t is_unsigned) {
    if (is_unsigned) {
        type |= 0x8000;
    }
    binary_set_integer(&mbind->type, type, 2, 1);
    binary_set_integer(&mbind->type_name, type, 2, 1);
    if (NULL == name) {
        _mysql_set_lenenc(&mbind->type_name, 0);
    } else {
        size_t lens = strlen(name);
        _mysql_set_lenenc(&mbind->type_name, lens);
        if (lens > 0) {
            binary_set_string(&mbind->type_name, name, lens);
        }
    }
}
void mysql_bind_nil(mysql_bind_ctx *mbind, const char *name) {
    _mysql_bind_bitmap(mbind, 1);
    _mysql_bind_type_name(mbind, MYSQL_TYPE_NULL, name, 0);
}
void mysql_bind_string(mysql_bind_ctx *mbind, const char *name, char *value, size_t lens) {
    if (NULL == value) {
        mysql_bind_nil(mbind, name);
    } else {
        _mysql_bind_bitmap(mbind, 0);
        _mysql_bind_type_name(mbind, MYSQL_TYPE_STRING, name, 0);
        _mysql_set_lenenc(&mbind->value, lens);
        if (lens > 0) {
            binary_set_string(&mbind->value, value, lens);
        }
    }
}
void mysql_bind_integer(mysql_bind_ctx *mbind, const char *name, int64_t value) {
    _mysql_bind_bitmap(mbind, 0);
    if (value >= SCHAR_MIN && value <= SCHAR_MAX) {
        _mysql_bind_type_name(mbind, MYSQL_TYPE_TINY, name, 0);
        binary_set_int8(&mbind->value, (int8_t)value);
        return;
    }
    if (value >= SHRT_MIN  && value <= SHRT_MAX) {
        _mysql_bind_type_name(mbind, MYSQL_TYPE_SHORT, name, 0);
        binary_set_integer(&mbind->value, (int16_t)value, sizeof(int16_t), 1);
        return;
    }
    if (value >= INT_MIN && value <= INT_MAX) {
        _mysql_bind_type_name(mbind, MYSQL_TYPE_LONG, name, 0);
        binary_set_integer(&mbind->value, (int32_t)value, sizeof(int32_t), 1);
        return;
    }
    _mysql_bind_type_name(mbind, MYSQL_TYPE_LONGLONG, name, 0);
    binary_set_integer(&mbind->value, value, sizeof(int64_t), 1);
}
void mysql_bind_uinteger(mysql_bind_ctx *mbind, const char *name, uint64_t value) {
    _mysql_bind_bitmap(mbind, 0);
    if (value <= UCHAR_MAX) {
        _mysql_bind_type_name(mbind, MYSQL_TYPE_TINY, name, 1);
        binary_set_uint8(&mbind->value, (uint8_t)value);
        return;
    }
    if (value <= USHRT_MAX) {
        _mysql_bind_type_name(mbind, MYSQL_TYPE_SHORT, name, 1);
        binary_set_integer(&mbind->value, (uint16_t)value, sizeof(uint16_t), 1);
        return;
    }
    if (value <= UINT_MAX) {
        _mysql_bind_type_name(mbind, MYSQL_TYPE_LONG, name, 1);
        binary_set_integer(&mbind->value, (uint32_t)value, sizeof(uint32_t), 1);
        return;
    }
    _mysql_bind_type_name(mbind, MYSQL_TYPE_LONGLONG, name, 1);
    binary_set_integer(&mbind->value, value, sizeof(uint64_t), 1);
}
void mysql_bind_double(mysql_bind_ctx *mbind, const char *name, double value) {
    _mysql_bind_bitmap(mbind, 0);
    if (value >= FLT_MIN && value <= FLT_MAX) {
        _mysql_bind_type_name(mbind, MYSQL_TYPE_FLOAT, name, 0);
        binary_set_float(&mbind->value, (float)value, 1);
        return;
    }
    _mysql_bind_type_name(mbind, MYSQL_TYPE_DOUBLE, name, 0);
    binary_set_double(&mbind->value, value, 1);
}
void mysql_bind_datetime(mysql_bind_ctx *mbind, const char *name, time_t ts) {
    _mysql_bind_bitmap(mbind, 0);
    _mysql_bind_type_name(mbind, MYSQL_TYPE_DATETIME, name, 0);
    struct tm *dt = localtime(&ts);
    if (0 == dt->tm_hour && 0 == dt->tm_min && 0 == dt->tm_sec) {
        binary_set_int8(&mbind->value, 4);
        binary_set_integer(&mbind->value, dt->tm_year + 1900, 2, 1);
        binary_set_int8(&mbind->value, dt->tm_mon + 1);
        binary_set_int8(&mbind->value, dt->tm_mday);
        return;
    }
    binary_set_int8(&mbind->value, 7);
    binary_set_integer(&mbind->value, dt->tm_year + 1900, 2, 1);
    binary_set_int8(&mbind->value, dt->tm_mon + 1);
    binary_set_int8(&mbind->value, dt->tm_mday);
    binary_set_int8(&mbind->value, dt->tm_hour);
    binary_set_int8(&mbind->value, dt->tm_min);
    binary_set_int8(&mbind->value, dt->tm_sec);
}
void mysql_bind_time(mysql_bind_ctx *mbind, const char *name,
    int8_t is_negative, int32_t days, int8_t hour, int8_t minute, int8_t second) {
    _mysql_bind_bitmap(mbind, 0);
    _mysql_bind_type_name(mbind, MYSQL_TYPE_TIME, name, 0);
    if (0 == days && 0 == hour && 0 == minute && 0 == second) {
        binary_set_int8(&mbind->value, 0);
        return;
    }
    binary_set_int8(&mbind->value, 8);
    binary_set_int8(&mbind->value, is_negative);
    binary_set_integer(&mbind->value, days, 4, 1);
    binary_set_int8(&mbind->value, hour);
    binary_set_int8(&mbind->value, minute);
    binary_set_int8(&mbind->value, second);
}
