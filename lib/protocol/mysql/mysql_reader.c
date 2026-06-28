#include "protocol/mysql/mysql_reader.h"
#include "protocol/mysql/mysql_parse.h"

mysql_reader_ctx *mysql_reader_init(mpack_ctx *mpack) {
    if ((MPACK_QUERY != mpack->pack_type && MPACK_STMT_EXECUTE != mpack->pack_type)
        || NULL == mpack->pack) {
        return NULL;
    }
    mysql_reader_ctx *reader = mpack->pack;
    // 将所有权从 mpack 转移给调用方，避免重复释放
    mpack->pack = NULL;
    mpack->_free_mpack = NULL;
    return reader;
}
void mysql_reader_free(mysql_reader_ctx *reader) {
    _mpack_reader_free(reader);
    FREE(reader);
}
size_t mysql_reader_size(mysql_reader_ctx *reader) {
    return array_size(&reader->arr_rows);
}
void mysql_reader_seek(mysql_reader_ctx *reader, size_t pos) {
    if (pos >= array_size(&reader->arr_rows)) {
        return;
    }
    reader->index = (int32_t)pos;
}
int32_t mysql_reader_eof(mysql_reader_ctx *reader) {
    return (reader->index >= (int32_t)array_size(&reader->arr_rows)) ? 1 : 0;
}
void mysql_reader_next(mysql_reader_ctx *reader) {
    if (reader->index < (int32_t)array_size(&reader->arr_rows)) {
        reader->index++;
    }
}
// 根据字段名在列描述数组中查找对应字段，返回字段指针并输出列索引
static mpack_field *_mysql_reader_field(mysql_reader_ctx *reader, const char *name, int32_t *pos) {
    for (int32_t i = 0; i < reader->field_count; i++) {
        if (0 == strcmp(reader->fields[i].name, name)) {
            *pos = i;
            return &reader->fields[i];
        }
    }
    return NULL;
}
// 根据字段名获取当前行中对应的 mpack_row，同时输出字段描述指针；NULL 字段或越界时设置 err
static mpack_row *_mysql_reader_row(mysql_reader_ctx *reader, const char *name, mpack_field **field, int32_t *err) {
    if (reader->index >= (int32_t)array_size(&reader->arr_rows)) {
        SET_PTR(err, ERR_FAILED);
        return NULL;
    }
    int32_t pos;
    mpack_field *column = _mysql_reader_field(reader, name, &pos);
    if (NULL == column) {
        SET_PTR(err, ERR_FAILED);
        return NULL;
    }
    mpack_row *row = *(mpack_row **)(array_at(&reader->arr_rows, (uint32_t)reader->index));
    if (row[pos].nil) {
        SET_PTR(err, 1); // 1 表示该字段值为 NULL
        return NULL;
    }
    *field = column;
    return &row[pos];
}
int64_t mysql_reader_integer(mysql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    mpack_field *field;
    mpack_row *row = _mysql_reader_row(reader, name, &field, err);
    if (NULL == row) {
        return 0;
    }
    if (MYSQL_TYPE_LONGLONG != field->type
        && MYSQL_TYPE_LONG != field->type
        && MYSQL_TYPE_INT24 != field->type
        && MYSQL_TYPE_SHORT != field->type
        && MYSQL_TYPE_YEAR != field->type
        && MYSQL_TYPE_TINY != field->type) {
        SET_PTR(err, ERR_FAILED);
        LOG_WARN("does not match required data type.");
        return 0;
    }
    if (MPACK_QUERY == reader->pack_type) {
        // 文本协议：字段值为字符串，需转换为整数
        char tmp[64];
        if (row->val.lens >= sizeof(tmp)) {
            SET_PTR(err, ERR_FAILED);
            LOG_WARN("parse failed.");
            return 0;
        }
        if (row->val.lens > 0) {
            memcpy(tmp, row->val.data, row->val.lens);
        }
        tmp[row->val.lens] = '\0';
        char *end;
        int64_t val = strtoll(tmp, &end, 10);
        if ((size_t)(end - tmp) != row->val.lens) {
            SET_PTR(err, ERR_FAILED);
            LOG_WARN("parse failed.");
            return 0;
        }
        return val;
    } else {
        // 二进制协议：字段值为原始二进制整数
        if (sizeof(int8_t) == row->val.lens) {
            return ((char *)row->val.data)[0];
        } else {
            return unpack_integer(row->val.data, (int32_t)row->val.lens, 1, 1);
        }
    }
}
uint64_t mysql_reader_uinteger(mysql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    mpack_field *field;
    mpack_row *row = _mysql_reader_row(reader, name, &field, err);
    if (NULL == row) {
        return 0;
    }
    if (MYSQL_TYPE_LONGLONG != field->type
        && MYSQL_TYPE_LONG != field->type
        && MYSQL_TYPE_INT24 != field->type
        && MYSQL_TYPE_SHORT != field->type
        && MYSQL_TYPE_YEAR != field->type
        && MYSQL_TYPE_TINY != field->type) {
        SET_PTR(err, ERR_FAILED);
        LOG_WARN("does not match required data type.");
        return 0;
    }
    if (MPACK_QUERY == reader->pack_type) {
        // 文本协议：字段值为字符串，需转换为无符号整数
        char tmp[64];
        if (row->val.lens >= sizeof(tmp)) {
            SET_PTR(err, ERR_FAILED);
            LOG_WARN("parse failed.");
            return 0;
        }
        if (row->val.lens > 0) {
            memcpy(tmp, row->val.data, row->val.lens);
        }
        tmp[row->val.lens] = '\0';
        char *end;
        uint64_t val = strtoull(tmp, &end, 10);
        if ((size_t)(end - tmp) != row->val.lens) {
            SET_PTR(err, ERR_FAILED);
            LOG_WARN("parse failed.");
            return 0;
        }
        return val;
    } else {
        // 二进制协议：字段值为原始二进制无符号整数
        if (sizeof(uint8_t) == row->val.lens) {
            return (uint8_t)(((char *)row->val.data)[0]);
        } else {
            return unpack_integer(row->val.data, (int32_t)row->val.lens, 1, 0);
        }
    }
}
// 文本协议浮点解析公共逻辑
static double _mysql_reader_parse_text_float(mpack_row *row, int32_t *err) {
    char tmp[128];
    if (row->val.lens >= sizeof(tmp)) {
        SET_PTR(err, ERR_FAILED);
        LOG_WARN("parse failed.");
        return 0.0;
    }
    if (row->val.lens > 0) {
        memcpy(tmp, row->val.data, row->val.lens);
    }
    tmp[row->val.lens] = '\0';
    char *end;
    double val = strtod(tmp, &end);
    if ((size_t)(end - tmp) != row->val.lens) {
        SET_PTR(err, ERR_FAILED);
        LOG_WARN("parse failed.");
        return 0.0;
    }
    return val;
}
float mysql_reader_float(mysql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    mpack_field *field;
    mpack_row *row = _mysql_reader_row(reader, name, &field, err);
    if (NULL == row) {
        return 0.0f;
    }
    if (MYSQL_TYPE_FLOAT != field->type) {
        SET_PTR(err, ERR_FAILED);
        LOG_WARN("does not match required data type.");
        return 0.0f;
    }
    if (MPACK_QUERY == reader->pack_type) {
        return (float)_mysql_reader_parse_text_float(row, err);
    } else {
        if (sizeof(float) != row->val.lens) {
            SET_PTR(err, ERR_FAILED);
            return 0.0f;
        }
        return unpack_float(row->val.data, 1);
    }
}
double mysql_reader_double(mysql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    mpack_field *field;
    mpack_row *row = _mysql_reader_row(reader, name, &field, err);
    if (NULL == row) {
        return 0.0;
    }
    if (MYSQL_TYPE_DOUBLE != field->type) {
        SET_PTR(err, ERR_FAILED);
        LOG_WARN("does not match required data type.");
        return 0.0;
    }
    if (MPACK_QUERY == reader->pack_type) {
        return _mysql_reader_parse_text_float(row, err);
    } else {
        if (sizeof(double) != row->val.lens) {
            SET_PTR(err, ERR_FAILED);
            return 0.0;
        }
        return unpack_double(row->val.data, 1);
    }
}
char *mysql_reader_string(mysql_reader_ctx *reader, const char *name, size_t *lens, int32_t *err) {
    SET_PTR(err, ERR_OK);
    mpack_field *field;
    mpack_row *row = _mysql_reader_row(reader, name, &field, err);
    if (NULL == row) {
        return NULL;
    }
    if (MYSQL_TYPE_STRING != field->type
        && MYSQL_TYPE_VARCHAR != field->type
        && MYSQL_TYPE_VAR_STRING != field->type
        && MYSQL_TYPE_ENUM != field->type
        && MYSQL_TYPE_SET != field->type
        && MYSQL_TYPE_LONG_BLOB != field->type
        && MYSQL_TYPE_MEDIUM_BLOB != field->type
        && MYSQL_TYPE_BLOB != field->type
        && MYSQL_TYPE_TINY_BLOB != field->type
        && MYSQL_TYPE_GEOMETRY != field->type
        && MYSQL_TYPE_BIT != field->type
        && MYSQL_TYPE_DECIMAL != field->type
        && MYSQL_TYPE_NEWDECIMAL != field->type
        && MYSQL_TYPE_JSON != field->type) {
        SET_PTR(err, ERR_FAILED);
        LOG_WARN("does not match required data type.");
        return NULL;
    }
    *lens = row->val.lens;
    return row->val.data;
}
static uint32_t _parse_usec_frac(const char *s) {
    uint32_t usec = 0;
    const char *dot = strchr(s, '.');
    if (NULL != dot) {
        dot++;
        int32_t mult = 100000;
        for (int32_t i = 0; i < 6 && dot[i] >= '0' && dot[i] <= '9'; i++, mult /= 10) {
            usec += (uint32_t)((dot[i] - '0') * mult);
        }
    }
    return usec;
}
int64_t mysql_reader_datetime(mysql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    mpack_field *field;
    mpack_row *row = _mysql_reader_row(reader, name, &field, err);
    if (NULL == row) {
        return 0;
    }
    if (MYSQL_TYPE_DATE != field->type
        && MYSQL_TYPE_DATETIME != field->type
        && MYSQL_TYPE_DATETIME2 != field->type
        && MYSQL_TYPE_TIMESTAMP != field->type
        && MYSQL_TYPE_TIMESTAMP2 != field->type) {
        SET_PTR(err, ERR_FAILED);
        LOG_WARN("does not match required data type.");
        return 0;
    }
    if (MPACK_QUERY == reader->pack_type) {
        char tmp[48];
        if (row->val.lens >= sizeof(tmp)) {
            SET_PTR(err, ERR_FAILED);
            return 0;
        }
        if (row->val.lens > 0) {
            memcpy(tmp, row->val.data, row->val.lens);
        }
        tmp[row->val.lens] = '\0';
        int32_t y, mo, d, h = 0, mi = 0, sec = 0;
        int32_t n = sscanf(tmp, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec);
        if (n != 3 && n != 6) {
            SET_PTR(err, ERR_FAILED);
            return 0;
        }
        uint32_t usec = _parse_usec_frac(tmp);
        struct tm dt = { 0 };
        dt.tm_year = y - 1900;
        dt.tm_mon = mo - 1;
        dt.tm_mday = d;
        dt.tm_hour = h;
        dt.tm_min = mi;
        dt.tm_sec = sec;
        time_t ts = mktime(&dt);
        if ((time_t)-1 == ts) {
            SET_PTR(err, ERR_FAILED);
            return 0;
        }
        return (int64_t)ts * 1000000LL + usec;
    } else {
        // 二进制协议：长度前缀 0=全零日期时间 4=仅日期 7=日期+时间 11=含微秒
        if (0 == row->val.lens) {
            return 0;
        }
        if (4 != row->val.lens && 7 != row->val.lens && 11 != row->val.lens) {
            SET_PTR(err, ERR_FAILED);
            return 0;
        }
        struct tm dt = { 0 };
        binary_ctx breader;
        binary_init(&breader, row->val.data, row->val.lens, 0);
        dt.tm_year = (int32_t)binary_get_integer(&breader, 2, 1) - 1900;
        dt.tm_mon = (int32_t)binary_get_int8(&breader) - 1;
        dt.tm_mday = (int32_t)binary_get_int8(&breader);
        if (row->val.lens >= 7) {
            dt.tm_hour = (int32_t)binary_get_int8(&breader);
            dt.tm_min = (int32_t)binary_get_int8(&breader);
            dt.tm_sec = (int32_t)binary_get_int8(&breader);
        }
        time_t ts = mktime(&dt);
        if ((time_t)-1 == ts) {
            SET_PTR(err, ERR_FAILED);
            return 0;
        }
        uint32_t usec = (11 == row->val.lens) ? (uint32_t)binary_get_integer(&breader, 4, 1) : 0;
        return (int64_t)ts * 1000000LL + usec;
    }
}
int32_t mysql_reader_time(mysql_reader_ctx *reader, const char *name, struct tm *time, uint32_t *usec, int32_t *err) {
    SET_PTR(err, ERR_OK);
    mpack_field *field;
    mpack_row *row = _mysql_reader_row(reader, name, &field, err);
    if (NULL == row) {
        return 0;
    }
    if (MYSQL_TYPE_TIME != field->type
        && MYSQL_TYPE_TIME2 != field->type) {
        SET_PTR(err, ERR_FAILED);
        LOG_WARN("does not match required data type.");
        return 0;
    }
    int32_t is_negative = 0;
    *time = (struct tm) { 0 };
    *usec = 0;
    if (MPACK_QUERY == reader->pack_type) {
        char tmp[48];
        if (row->val.lens >= sizeof(tmp)) {
            SET_PTR(err, ERR_FAILED);
            return 0;
        }
        if (row->val.lens > 0) {
            memcpy(tmp, row->val.data, row->val.lens);
        }
        tmp[row->val.lens] = '\0';
        char *p = tmp;
        if ('-' == *p) {
            is_negative = 1;
            p++;
        }
        int32_t h = 0, mi = 0, sec = 0;
        if (3 != sscanf(p, "%d:%d:%d", &h, &mi, &sec)) {
            SET_PTR(err, ERR_FAILED);
            return 0;
        }
        *usec = _parse_usec_frac(p);
        time->tm_mday = h / 24;
        time->tm_hour = h % 24;
        time->tm_min = mi;
        time->tm_sec = sec;
    } else {
        // 二进制协议：长度前缀 0=零时间 8=天+时分秒 12=含微秒
        if (0 == row->val.lens) {
            return 0;
        }
        if (8 != row->val.lens && 12 != row->val.lens) {
            SET_PTR(err, ERR_FAILED);
            return 0;
        }
        binary_ctx breader;
        binary_init(&breader, row->val.data, row->val.lens, 0);
        is_negative = (int32_t)binary_get_int8(&breader);
        time->tm_mday = (int32_t)binary_get_integer(&breader, 4, 1);
        time->tm_hour = (int32_t)binary_get_int8(&breader);
        time->tm_min = (int32_t)binary_get_int8(&breader);
        time->tm_sec = (int32_t)binary_get_int8(&breader);
        if (12 == row->val.lens) {
            *usec = (uint32_t)binary_get_integer(&breader, 4, 1);
        }
    }
    return is_negative;
}
