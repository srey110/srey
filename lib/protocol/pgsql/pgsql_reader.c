#include "protocol/pgsql/pgsql_reader.h"
#include "protocol/pgsql/pgsql_parse.h"

pgsql_reader_ctx *pgsql_reader_init(pgpack_ctx *pgpack, int16_t format) {
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
static pgpack_field *_pgsql_reader_field(pgsql_reader_ctx *reader, const char *name, int32_t *pos) {
    for (int16_t i = 0; i < reader->field_count; i++) {
        if (0 == strcmp(reader->fields[i].name, name)) {
            *pos = i;
            return &reader->fields[i];
        }
    }
    return NULL;
}
pgpack_row *pgsql_reader_read(pgsql_reader_ctx *reader, const char *name, pgpack_field **field) {
    if (reader->index >= (int32_t)arr_ptr_size(&reader->arr_rows)) {
        return NULL;
    }
    int32_t pos;
    pgpack_field *column = _pgsql_reader_field(reader, name, &pos);
    if (NULL == column) {
        return NULL;
    }
    pgpack_row *row = *arr_ptr_at(&reader->arr_rows, (uint32_t)reader->index);
    if (NULL != field) {
        *field = column;
    }
    return &row[pos];
}
