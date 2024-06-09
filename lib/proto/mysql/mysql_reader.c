#include "proto/mysql/mysql_reader.h"
#include "proto/mysql/mysql_parse.h"

mysql_reader_ctx *mysql_reader_init(mpack_ctx *mpack) {
    if ((MPACK_QUERY != mpack->pack_type && MPACK_STMT_EXECUTE != mpack->pack_type)
        || NULL == mpack->pack) {
        return NULL;
    }
    mysql_reader_ctx *reader = mpack->pack;
    mpack->pack = NULL;
    mpack->_free_mpack = NULL;
    return reader;
}
void mysql_reader_free(mysql_reader_ctx *reader) {
    _mpack_reader_free(reader);
    FREE(reader);
}
size_t mysql_reader_size(mysql_reader_ctx *reader) {
    return arr_ptr_size(&reader->arr_rows);
}
void mysql_reader_seek(mysql_reader_ctx *reader, size_t pos) {
    if (pos >= arr_ptr_size(&reader->arr_rows)) {
        return;
    }
    reader->index = (int32_t)pos;
}
int32_t mysql_reader_eof(mysql_reader_ctx *reader) {
    return (reader->index >= (int32_t)arr_ptr_size(&reader->arr_rows)) ? 1 : 0;
}
void mysql_reader_next(mysql_reader_ctx *reader) {
    if (reader->index < (int32_t)arr_ptr_size(&reader->arr_rows)) {
        reader->index++;
    }
}
static mpack_field *_mysql_reader_field(mysql_reader_ctx *reader, const char *name, int32_t *pos) {
    for (int32_t i = 0; i < reader->field_count; i++) {
        if (0 == strcmp(reader->fields[i].name, name)) {
            *pos = i;
            return &reader->fields[i];
        }
    }
    return NULL;
}
static mpack_row *_mysql_reader_row(mysql_reader_ctx *reader, const char *name, mpack_field **field, int32_t *err) {
    if (reader->index >= (int32_t)arr_ptr_size(&reader->arr_rows)) {
        if (NULL != err) {
            *err = ERR_FAILED;
        }
        return NULL;
    }
    int32_t pos;
    mpack_field *column = _mysql_reader_field(reader, name, &pos);
    if (NULL == column) {
        if (NULL != err) {
            *err = ERR_FAILED;
        }
        return NULL;
    }
    mpack_row *row = *(mpack_row **)(arr_ptr_at(&reader->arr_rows, (uint32_t)reader->index));
    if (row[pos].nil) {
        if (NULL != err) {
            *err = 1;
        }
        return NULL;
    }
    *field = column;
    return &row[pos];
}
int64_t mysql_reader_integer(mysql_reader_ctx *reader, const char *name, int32_t *err) {
    if (NULL != err) {
        *err = ERR_OK;
    }
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
        if (NULL != err) {
            *err = ERR_FAILED;
        }
        LOG_WARN("does not match required data type.");
        return 0;
    }
    if (MPACK_QUERY == reader->pack_type) {
        char tmp[64];
        memcpy(tmp, row->val.data, row->val.lens);
        tmp[row->val.lens] = '\0';
        char *end;
        int64_t val = strtoll(tmp, &end, 10);
        if ((size_t)(end - tmp) != row->val.lens) {
            if (NULL != err) {
                *err = ERR_FAILED;
            }
            LOG_WARN("parse failed.");
            return 0;
        }
        return val;
    } else {
        if (sizeof(int8_t) == row->val.lens) {
            return ((char *)row->val.data)[0];
        } else {
            return unpack_integer(row->val.data, (int32_t)row->val.lens, 1, 1);
        }
    }
}
uint64_t mysql_reader_uinteger(mysql_reader_ctx *reader, const char *name, int32_t *err) {
    if (NULL != err) {
        *err = ERR_OK;
    }
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
        if (NULL != err) {
            *err = ERR_FAILED;
        }
        LOG_WARN("does not match required data type.");
        return 0;
    }
    if (MPACK_QUERY == reader->pack_type) {
        char tmp[64];
        memcpy(tmp, row->val.data, row->val.lens);
        tmp[row->val.lens] = '\0';
        char *end;
        uint64_t val = strtoull(tmp, &end, 10);
        if ((size_t)(end - tmp) != row->val.lens) {
            if (NULL != err) {
                *err = ERR_FAILED;
            }
            LOG_WARN("parse failed.");
            return 0;
        }
        return val;
    } else {
        if (sizeof(uint8_t) == row->val.lens) {
            return (uint8_t)(((char *)row->val.data)[0]);
        } else {
            return unpack_integer(row->val.data, (int32_t)row->val.lens, 1, 0);
        }
    }
}
double mysql_reader_double(mysql_reader_ctx *reader, const char *name, int32_t *err) {
    if (NULL != err) {
        *err = ERR_OK;
    }
    mpack_field *field;
    mpack_row *row = _mysql_reader_row(reader, name, &field, err);
    if (NULL == row) {
        return 0.0;
    }
    if (MYSQL_TYPE_DOUBLE != field->type
        && MYSQL_TYPE_FLOAT != field->type) {
        if (NULL != err) {
            *err = ERR_FAILED;
        }
        LOG_WARN("does not match required data type.");
        return 0.0;
    }
    if (MPACK_QUERY == reader->pack_type) {
        char tmp[128];
        memcpy(tmp, row->val.data, row->val.lens);
        tmp[row->val.lens] = '\0';
        char *end;
        double val = strtod(tmp, &end);
        if ((size_t)(end - tmp) != row->val.lens) {
            if (NULL != err) {
                *err = ERR_FAILED;
            }
            LOG_WARN("parse failed.");
            return 0.0;
        }
        return val;
    } else {
        if (sizeof(double) == row->val.lens) {
            return unpack_double(row->val.data, 1);
        } else {
            return unpack_float(row->val.data, 1);
        }
    }
}
char *mysql_reader_string(mysql_reader_ctx *reader, const char *name, size_t *lens, int32_t *err) {
    if (NULL != err) {
        *err = ERR_OK;
    }
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
        if (NULL != err) {
            *err = ERR_FAILED;
        }
        LOG_WARN("does not match required data type.");
        return NULL;
    }
    *lens = row->val.lens;
    return row->val.data;
}
uint64_t mysql_reader_datetime(mysql_reader_ctx *reader, const char *name, int32_t *err) {
    if (NULL != err) {
        *err = ERR_OK;
    }
    mpack_field *field;
    mpack_row *row = _mysql_reader_row(reader, name, &field, err);
    if (NULL == row) {
        return 0;
    }
    if (0 == row->val.lens) {
        if (NULL != err) {
            *err = ERR_FAILED;
        }
        return 0;
    }
    if (MYSQL_TYPE_DATE != field->type
        && MYSQL_TYPE_DATETIME != field->type
        && MYSQL_TYPE_TIMESTAMP != field->type) {
        if (NULL != err) {
            *err = ERR_FAILED;
        }
        LOG_WARN("does not match required data type.");
        return 0;
    }
    if (MPACK_QUERY == reader->pack_type) {
        char tmp[64];
        memcpy(tmp, row->val.data, row->val.lens);
        tmp[row->val.lens] = '\0';
        return strtots(tmp, "%Y-%m-%d %H:%M:%S");
    } else {
        struct tm dt;
        ZERO(&dt, sizeof(struct tm));
        binary_ctx breader;
        binary_init(&breader, row->val.data, row->val.lens, 0);
        dt.tm_year = (int32_t)binary_get_integer(&breader, 2, 1) - 1900;
        dt.tm_mon = (int32_t)binary_get_int8(&breader) - 1;
        dt.tm_mday = (int32_t)binary_get_int8(&breader);
        dt.tm_hour = (int32_t)binary_get_int8(&breader);
        dt.tm_min = (int32_t)binary_get_int8(&breader);
        dt.tm_sec = (int32_t)binary_get_int8(&breader);
        return mktime(&dt);
    }
}
int32_t mysql_reader_time(mysql_reader_ctx *reader, const char *name, struct tm *time, int32_t *err) {
    if (NULL != err) {
        *err = ERR_OK;
    }
    mpack_field *field;
    mpack_row *row = _mysql_reader_row(reader, name, &field, err);
    if (NULL == row) {
        return 0;
    }
    if (0 == row->val.lens) {
        if (NULL != err) {
            *err = ERR_FAILED;
        }
        return 0;
    }
    if (MYSQL_TYPE_TIME != field->type) {
        if (NULL != err) {
            *err = ERR_FAILED;
        }
        LOG_WARN("does not match required data type.");
        return 0;
    }
    int32_t is_negative = 0;
    if (MPACK_QUERY == reader->pack_type) {
        char tmp[64];
        memcpy(tmp, row->val.data, row->val.lens);
        tmp[row->val.lens] = '\0';
        char *end;
        int32_t val = strtol(tmp, &end, 10);
        if (val < 0) {
            is_negative = 1;
            val *= -1;
        }
        time->tm_mday = val / 24;
        time->tm_hour = val % 24;
        time->tm_min = strtol(end + 1, &end, 10);
        time->tm_sec = strtol(end + 1, &end, 10);
    } else {
        binary_ctx breader;
        binary_init(&breader, row->val.data, row->val.lens, 0);
        is_negative = (int32_t)binary_get_int8(&breader);
        time->tm_mday = (int32_t)binary_get_integer(&breader, 4, 1);
        time->tm_hour = (int32_t)binary_get_int8(&breader);
        time->tm_min = (int32_t)binary_get_int8(&breader);
        time->tm_sec = (int32_t)binary_get_int8(&breader);
    }
    return is_negative;
}
