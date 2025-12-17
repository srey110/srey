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

