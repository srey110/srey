#include "protocol/pgsql/pgsql_reader.h"
#include "protocol/pgsql/pgsql_parse.h"

pgsql_reader_ctx *pgsql_reader_init(pgpack_ctx *pgpack, pgpack_format format) {
    if (NULL == pgpack->pack
        || PGPACK_OK != pgpack->type) {
        return NULL;
    }
    pgsql_reader_ctx *reader = pgpack->pack;
    reader->format = format;
    pgpack->pack = NULL;
    pgpack->_free_pgpack = NULL;
    return reader;
}
void pgsql_reader_free(pgsql_reader_ctx *reader) {
    _pgpack_reader_free(reader);
    FREE(reader);
}
size_t pgsql_reader_size(pgsql_reader_ctx *reader) {
    return arr_ptr_size(&reader->arr_rows);
}
void pgsql_reader_seek(pgsql_reader_ctx *reader, size_t pos) {
    if (pos >= arr_ptr_size(&reader->arr_rows)) {
        return;
    }
    reader->index = (int32_t)pos;
}
int32_t pgsql_reader_eof(pgsql_reader_ctx *reader) {
    return (reader->index >= (int32_t)arr_ptr_size(&reader->arr_rows)) ? 1 : 0;
}
void pgsql_reader_next(pgsql_reader_ctx *reader) {
    if (reader->index < (int32_t)arr_ptr_size(&reader->arr_rows)) {
        reader->index++;
    }
}
pgpack_row *pgsql_reader_index(pgsql_reader_ctx *reader, int16_t index, pgpack_field **field) {
    if (reader->index >= (int32_t)arr_ptr_size(&reader->arr_rows)
        || (index < 0 || index >= reader->field_count)) {
        return NULL;
    }
    pgpack_row *row = *arr_ptr_at(&reader->arr_rows, (uint32_t)reader->index);
    if (NULL != field
        && NULL != reader->fields) {
        *field = &reader->fields[index];
    }
    return &row[index];
}
static int16_t _pgsql_reader_index(pgsql_reader_ctx *reader, const char *name) {
    for (int16_t i = 0; i < reader->field_count; i++) {
        if (0 == strcmp(reader->fields[i].name, name)) {
            return i;
        }
    }
    return ERR_FAILED;
}
pgpack_row *pgsql_reader_name(pgsql_reader_ctx *reader, const char *name, pgpack_field **field) {
    if (NULL == reader->fields) {
        return NULL;
    }
    int16_t index = _pgsql_reader_index(reader, name);
    if (ERR_FAILED == index) {
        return NULL;
    }
    return pgsql_reader_index(reader, index, field);
}
int32_t pgsql_reader_bool(pgsql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    pgpack_field *field;
    pgpack_row *row = pgsql_reader_name(reader, name, &field);
    if (NULL == row) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    if (BOOLOID != field->type_oid) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    if (-1 == row->lens) {
        SET_PTR(err, 1);
        return 0;
    }
    if (FORMAT_TEXT == reader->format) {
        static const char *_pgsql_true[] = { "t", "true", "y", "yes", "on", "1" };
        size_t n = ARRAY_SIZE(_pgsql_true);
        for (int32_t i = 0; i < n; i++) {
            if (strlen(_pgsql_true[i]) == row->lens
                && 0 == _memicmp(row->val, _pgsql_true[i], row->lens)) {
                return 1;
            }
        }
        return 0;
    }
    return row->val[0];
}
int64_t pgsql_reader_integer(pgsql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    pgpack_field *field;
    pgpack_row *row = pgsql_reader_name(reader, name, &field);
    if (NULL == row) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    if (INT2OID != field->type_oid
        && INT4OID != field->type_oid
        && INT8OID != field->type_oid) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    if (-1 == row->lens) {
        SET_PTR(err, 1);
        return 0;
    }
    if (FORMAT_TEXT == reader->format) {
        char tmp[64];
        memcpy(tmp, row->val, row->lens);
        tmp[row->lens] = '\0';
        char *end;
        int64_t val = strtoll(tmp, &end, 10);
        if ((int32_t)(end - tmp) != row->lens) {
            SET_PTR(err, ERR_FAILED);
            LOG_WARN("parse failed.");
            return 0;
        }
        return val;
    }
    return unpack_integer(row->val, row->lens, 0, 1);
}
double pgsql_reader_double(pgsql_reader_ctx *reader, const char *name, int32_t *err) {
    SET_PTR(err, ERR_OK);
    pgpack_field *field;
    pgpack_row *row = pgsql_reader_name(reader, name, &field);
    if (NULL == row) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    if (FLOAT4OID != field->type_oid
        && FLOAT8OID != field->type_oid) {
        SET_PTR(err, ERR_FAILED);
        return 0;
    }
    if (-1 == row->lens) {
        SET_PTR(err, 1);
        return 0;
    }
    if (FORMAT_TEXT == reader->format) {
        char tmp[128];
        memcpy(tmp, row->val, row->lens);
        tmp[row->lens] = '\0';
        char *end;
        double val = strtod(tmp, &end);
        if ((int32_t)(end - tmp) != row->lens) {
            SET_PTR(err, ERR_FAILED);
            LOG_WARN("parse failed.");
            return 0.0;
        }
        return val;
    }
    if (sizeof(double) == row->lens) {
        return unpack_double(row->val, 0);
    } else {
        return unpack_float(row->val, 0);
    }
}
