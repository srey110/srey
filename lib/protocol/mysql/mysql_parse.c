#include "protocol/mysql/mysql_parse.h"
#include "protocol/mysql/mysql_utils.h"
#include "protocol/prots.h"
#include "utils/utils.h"

typedef enum RST_STATUS {
    RST_FIELD = 0x01,
    RST_ROW
}RST_STATUS;
typedef enum STMT_PREPARE_STATUS {
    STMT_PREPARE_PARAMS = 0x01,
    STMT_PREPARE_FIELD
}STMT_PREPARE_STATUS;

static int32_t _mysql_head(mysql_ctx *mysql, buffer_ctx *buf, size_t *payload_lens) {
    size_t size = buffer_size(buf);
    if (size < MYSQL_HEAD_LENS) {
        return ERR_FAILED;
    }
    char head[MYSQL_HEAD_LENS];
    ASSERTAB(sizeof(head) == buffer_copyout(buf, 0, head, sizeof(head)), "copy buffer failed.");
    *payload_lens = (size_t)unpack_integer(head, 3, 1, 0);
    if (size < *payload_lens + sizeof(head)) {
        return ERR_FAILED;
    }
    mysql->id = head[3];
    ASSERTAB(sizeof(head) == buffer_drain(buf, sizeof(head)), "drain buffer failed.");
    return ERR_OK;
}
char *_mysql_payload(mysql_ctx *mysql, buffer_ctx *buf, size_t *payload_lens, int32_t *status) {
    if (ERR_OK != _mysql_head(mysql, buf, payload_lens)) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    char *payload;
    MALLOC(payload, *payload_lens);
    ASSERTAB(*payload_lens == buffer_remove(buf, payload, *payload_lens), "copy buffer failed.");
    return payload;
}
void _mpack_ok(mysql_ctx *mysql, binary_ctx *breader, mpack_ok *ok) {
    uint64_t size = _mysql_get_lenenc(breader);
    ok->affected_rows = (int64_t)size;
    size = _mysql_get_lenenc(breader);
    ok->last_insert_id = (int64_t)size;
    ok->status_flags = (int16_t)binary_get_integer(breader, 2, 1);
    ok->warnings = (int16_t)binary_get_integer(breader, 2, 1);
    binary_get_skip(breader, breader->size - breader->offset);
    mysql->last_id = ok->last_insert_id;
    mysql->affected_rows = ok->affected_rows;
}
void _mpack_eof(binary_ctx *breader, mpack_eof *eof) {
    eof->warnings = (int16_t)binary_get_integer(breader, 2, 1);
    eof->status_flags = (int16_t)binary_get_integer(breader, 2, 1);
}
void _mpack_err(mysql_ctx *mysql, binary_ctx *breader, mpack_err *err) {
    err->error_code = (int16_t)binary_get_integer(breader, 2, 1);
    binary_get_skip(breader, 6);//sql_state_marker sql_state
    err->error_msg.lens = breader->size - breader->offset;
    mysql->error_code = err->error_code;
    if (err->error_msg.lens > 0) {
        err->error_msg.data = binary_get_string(breader, err->error_msg.lens);
        size_t lens = err->error_msg.lens <= sizeof(mysql->error_msg) - 1 ? err->error_msg.lens : sizeof(mysql->error_msg) - 1;
        memcpy(mysql->error_msg, err->error_msg.data, lens);
        mysql->error_msg[lens] = '\0';
    } else {
        mysql->error_msg[0] = '\0';
    }
}
static mpack_ctx *_mpack_new(mysql_ctx *mysql, char *payload) {
    mpack_ctx *mpack;
    CALLOC(mpack, 1, sizeof(mpack_ctx));
    mpack->sequence_id = mysql->id;
    mpack->payload = payload;
    return mpack;
}
static mpack_ctx *_selectdb_response(mysql_ctx *mysql, binary_ctx *breader) {
    mpack_ctx *mpack = _mpack_new(mysql, breader->data);
    if (MYSQL_OK == binary_get_uint8(breader)) {
        mpack->pack_type = MPACK_OK;
        MALLOC(mpack->pack, sizeof(mpack_ok));
        _mpack_ok(mysql, breader, mpack->pack);
    } else {
        mpack->pack_type = MPACK_ERR;
        MALLOC(mpack->pack, sizeof(mpack_err));
        _mpack_err(mysql, breader, mpack->pack);
    }
    mysql->cur_cmd = 0;
    return mpack;
}
static mpack_ctx *_ping_response(mysql_ctx *mysql, binary_ctx *breader) {
    mpack_ctx *mpack = _mpack_new(mysql, breader->data);
    binary_get_skip(breader, 1);
    mpack->pack_type = MPACK_OK;
    MALLOC(mpack->pack, sizeof(mpack_ok));
    _mpack_ok(mysql, breader, mpack->pack);
    mysql->cur_cmd = 0;
    return mpack;
}
void _mpack_reader_free(void *pack) {
    mpack_row *rows;
    mysql_reader_ctx *reader = pack;
    for (uint32_t i = 0; i < arr_ptr_size(&reader->arr_rows); i++) {
        rows = *(mpack_row **)(arr_ptr_at(&reader->arr_rows, i));
        FREE(rows->payload);
        FREE(rows);
    }
    arr_ptr_free(&reader->arr_rows);
    FREE(reader->fields);
}
static void _mpack_reader_new(mysql_ctx *mysql, binary_ctx *breader, mpack_type pktype) {
    mysql->mpack = _mpack_new(mysql, NULL);
    mysql_reader_ctx *reader;
    CALLOC(reader, 1, sizeof(mysql_reader_ctx));
    reader->pack_type = pktype;
    reader->field_count = (int32_t)_mysql_get_lenenc(breader);//column_count
    if (reader->field_count > 0) {
        CALLOC(reader->fields, 1, sizeof(mpack_field) * (size_t)reader->field_count);
    }
    arr_ptr_init(&reader->arr_rows, ONEK);
    mysql->mpack->pack = reader;
    mysql->mpack->_free_mpack = _mpack_reader_free;
    mysql->mpack->pack_type = pktype;
}
static int32_t _mpack_more_data(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    size_t payload_lens;
    char *payload = _mysql_payload(mysql, buf, &payload_lens, status);
    if (NULL == payload) {
        return ERR_FAILED;
    }
    binary_init(breader, payload, payload_lens, 0);
    return ERR_OK;
}
static int32_t _mpack_check_final(binary_ctx *breader, int32_t *status) {
    binary_get_skip(breader, 1);
    mpack_eof eof;
    _mpack_eof(breader, &eof);
    if (BIT_CHECK(eof.status_flags, SERVER_MORE_RESULTS_EXISTS)) {
        BIT_SET(*status, PROT_MOREDATA);
        return ERR_FAILED;
    }
    return ERR_OK;
}
static void _mpack_parse_text_row(mysql_reader_ctx *reader, binary_ctx *breader) {
    mpack_row *row;
    CALLOC(row, 1, sizeof(mpack_row) * (size_t)reader->field_count);
    row->payload = breader->data;
    for (int32_t i = 0; i < reader->field_count; i++) {
        if (0xfb == (uint8_t)(binary_at(breader, breader->offset)[0])) {
            row[i].nil = 1;
            binary_get_skip(breader, 1);
            continue;
        }
        row[i].val.lens = (size_t)_mysql_get_lenenc(breader);
        if (row[i].val.lens > 0) {
            row[i].val.data = binary_get_string(breader, row[i].val.lens);
        }
    }
    arr_ptr_push_back(&reader->arr_rows, (void **)&row);
}
static int32_t _mpack_parse_binary_row(mysql_reader_ctx *reader, binary_ctx *breader) {
    int32_t off;
    mpack_row *row;
    CALLOC(row, 1, sizeof(mpack_row) * (size_t)reader->field_count);
    row->payload = breader->data;
    char *bitmap = binary_get_string(breader, ((reader->field_count + 9) / 8));
    for (int32_t i = 0; i < reader->field_count; i++) {
        off = i + 2;
        if (BIT_CHECK(bitmap[(off / 8)], (1 << (off % 8)))) {
            row[i].nil = 1;
            continue;
        }
        switch (reader->fields[i].type) {
        case MYSQL_TYPE_LONGLONG:
            row[i].val.lens = sizeof(int64_t);
            break;
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_INT24:
            row[i].val.lens = sizeof(int32_t);
            break;
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_YEAR:
            row[i].val.lens = sizeof(int16_t);
            break;
        case MYSQL_TYPE_TINY:
            row[i].val.lens = sizeof(int8_t);
            break;
        case MYSQL_TYPE_DOUBLE:
            row[i].val.lens = sizeof(double);
            break;
        case MYSQL_TYPE_FLOAT:
            row[i].val.lens = sizeof(float);
            break;
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_TIME:
            row[i].val.lens = (size_t)binary_get_int8(breader);
            break;
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_ENUM:
        case MYSQL_TYPE_SET:
        case MYSQL_TYPE_LONG_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_GEOMETRY:
        case MYSQL_TYPE_BIT:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
        case MYSQL_TYPE_JSON:
            row[i].val.lens = (size_t)_mysql_get_lenenc(breader);
            break;
        default:
            LOG_WARN("unknow data type %d.", (int32_t)reader->fields[i].type);
            FREE(row);
            return ERR_FAILED;
        }
        if (row[i].val.lens > 0) {
            row[i].val.data = binary_get_string(breader, row[i].val.lens);
        }
    }
    arr_ptr_push_back(&reader->arr_rows, (void **)&row);
    return ERR_OK;
}
static mpack_ctx *_mpack_reader_rows(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    uint8_t first;
    mysql_reader_ctx *reader = mysql->mpack->pack;
    for (;;) {
        first = (uint8_t)(binary_at(breader, breader->offset)[0]);
        if (MYSQL_EOF == first) {
            if (ERR_OK != _mpack_check_final(breader, status)) {
                FREE(breader->data);
                return NULL;
            }
            FREE(breader->data);
            mpack_ctx *mpack = mysql->mpack; //row解析完成
            mysql->mpack = NULL;
            mysql->cur_cmd = 0;
            return mpack;
        }
        if (MPACK_QUERY == mysql->mpack->pack_type) {
            _mpack_parse_text_row(reader, breader);
        } else {
            if (0x00 != first) {
                BIT_SET(*status, PROT_ERROR);
                return NULL;
            }
            binary_get_skip(breader, 1);
            if (ERR_OK != _mpack_parse_binary_row(reader, breader)) {
                BIT_SET(*status, PROT_ERROR);
                return NULL;
            }
        }
        if (ERR_OK != _mpack_more_data(mysql, buf, breader, status)) {
            return NULL;
        }
    }
    return NULL;
}
static void _mpack_parse_field(binary_ctx *breader, mpack_field *field) {
    char *val;
    uint64_t lens = _mysql_get_lenenc(breader);
    binary_get_skip(breader, (size_t)lens);//catalog
    lens = _mysql_get_lenenc(breader);
    if (lens > 0) {
        val = binary_get_string(breader, (size_t)lens);
        memcpy(field->schema, val, (size_t)lens);
    }
    lens = _mysql_get_lenenc(breader);
    if (lens > 0) {
        val = binary_get_string(breader, (size_t)lens);
        memcpy(field->table, val, (size_t)lens);
    }
    lens = _mysql_get_lenenc(breader);
    if (lens > 0) {
        val = binary_get_string(breader, (size_t)lens);
        memcpy(field->org_table, val, (size_t)lens);
    }
    lens = _mysql_get_lenenc(breader);
    if (lens > 0) {
        val = binary_get_string(breader, (size_t)lens);
        memcpy(field->name, val, (size_t)lens);
    }
    lens = _mysql_get_lenenc(breader);
    if (lens > 0) {
        val = binary_get_string(breader, (size_t)lens);
        memcpy(field->org_name, val, (size_t)lens);
    }
    _mysql_get_lenenc(breader);//length of fixed length fields
    field->character = (int16_t)binary_get_integer(breader, 2, 1);
    field->field_lens = (int32_t)binary_get_integer(breader, 4, 1);
    field->type = binary_get_uint8(breader);
    field->flags = (uint16_t)binary_get_uinteger(breader, 2, 1);
    field->decimals = binary_get_uint8(breader);
}
static mpack_ctx *_mpack_reader_fileds(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    mysql_reader_ctx *reader = mysql->mpack->pack;
    for (;;) {
        if (MYSQL_EOF == (uint8_t)(binary_at(breader, breader->offset)[0])) {
            if (ERR_OK != _mpack_check_final(breader, status)) {
                FREE(breader->data);
                return NULL;
            }
            FREE(breader->data);
            reader->index = 0;
            mysql->parse_status = RST_ROW;//filed解析完成
            break;
        }
        _mpack_parse_field(breader, &reader->fields[reader->index]);
        FREE(breader->data);
        ++reader->index;
        if (ERR_OK != _mpack_more_data(mysql, buf, breader, status)) {
            return NULL;
        }
    }
    if (ERR_OK != _mpack_more_data(mysql, buf, breader, status)) {
        return NULL;
    }
    return _mpack_reader_rows(mysql, buf, breader, status);
}
static mpack_ctx *_mpack_reader(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    mpack_ctx *mpack = NULL;
    switch (mysql->parse_status) {
    case RST_FIELD:
        mpack = _mpack_reader_fileds(mysql, buf, breader, status);
        break;
    case RST_ROW:
        mpack = _mpack_reader_rows(mysql, buf, breader, status);
        break;
    default:
        break;
    }
    return mpack;
}
static mpack_ctx *_query_response(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    mpack_ctx *mpack = NULL;
    if (0 == mysql->parse_status) {
        switch ((uint8_t)(binary_at(breader, 0)[0])) {
        case MYSQL_OK:
            binary_get_skip(breader, 1);
            mpack = _mpack_new(mysql, breader->data);
            mpack->pack_type = MPACK_OK;
            MALLOC(mpack->pack, sizeof(mpack_ok));
            _mpack_ok(mysql, breader, mpack->pack);
            mysql->cur_cmd = 0;
            break;
        case MYSQL_ERR:
            binary_get_skip(breader, 1);
            mpack = _mpack_new(mysql, breader->data);
            mpack->pack_type = MPACK_ERR;
            MALLOC(mpack->pack, sizeof(mpack_err));
            _mpack_err(mysql, breader, mpack->pack);
            mysql->cur_cmd = 0;
            break;
        case MYSQL_LOCAL_INFILE:
            BIT_SET(*status, PROT_ERROR);
            FREE(breader->data);
            break;
        default:
            _mpack_reader_new(mysql, breader, MPACK_QUERY);//读取字段数
            FREE(breader->data);
            mysql->parse_status = RST_FIELD;
            if (ERR_OK != _mpack_more_data(mysql, buf, breader, status)) {
                break;
            }
            mpack = _mpack_reader(mysql, buf, breader, status);
            break;
        }
    } else {
        mpack = _mpack_reader(mysql, buf, breader, status);
    }
    return mpack;
}
static mpack_ctx *_mpack_stmt(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    mysql_stmt_ctx *stmt = mysql->mpack->pack;
    for (;;) {
        if (MYSQL_EOF == (uint8_t)(binary_at(breader, breader->offset)[0])) {
            if (ERR_OK != _mpack_check_final(breader, status)) {
                FREE(breader->data);
                return NULL;
            }
            FREE(breader->data);
            if (STMT_PREPARE_PARAMS == mysql->parse_status) {
                if (stmt->field_count > 0) {
                    mysql->parse_status = STMT_PREPARE_FIELD;
                    stmt->index = 0;
                    if (ERR_OK != _mpack_more_data(mysql, buf, breader, status)) {
                        return NULL;
                    }
                    continue;
                } else {
                    mpack_ctx *mpack = mysql->mpack;
                    mysql->mpack = NULL;
                    mysql->cur_cmd = 0;
                    return mpack;
                }
            } else {
                mpack_ctx *mpack = mysql->mpack;
                mysql->mpack = NULL;
                mysql->cur_cmd = 0;
                return mpack;
            }
        }
        if (STMT_PREPARE_PARAMS == mysql->parse_status) {
            _mpack_parse_field(breader, &stmt->params[stmt->index]);
        } else {
            _mpack_parse_field(breader, &stmt->fields[stmt->index]);
        }
        FREE(breader->data);
        ++stmt->index;
        if (ERR_OK != _mpack_more_data(mysql, buf, breader, status)) {
            return NULL;
        }
    }
    return NULL;
}
mysql_stmt_ctx *mysql_stmt_init(mpack_ctx *mpack) {
    if (NULL == mpack
        || NULL == mpack->pack
        || MPACK_STMT_PREPARE != mpack->pack_type) {
        return NULL;
    }
    mysql_stmt_ctx *stmt = mpack->pack;
    mpack->pack = NULL;
    mpack->_free_mpack = NULL;
    return stmt;
}
void _mpack_stm_free(void *pack) {
    mysql_stmt_ctx *stmt = pack;
    FREE(stmt->params);
    FREE(stmt->fields);
}
static void _mpack_stmt_new(mysql_ctx *mysql, binary_ctx *breader) {
    mysql->mpack = _mpack_new(mysql, NULL);
    mysql->mpack->pack_type = MPACK_STMT_PREPARE;
    mysql_stmt_ctx *stmt;
    CALLOC(stmt, 1, sizeof(mysql_stmt_ctx));
    stmt->mysql = mysql;
    stmt->stmt_id = (int32_t)binary_get_integer(breader, 4, 1);
    stmt->field_count = (uint16_t)binary_get_uinteger(breader, 2, 1);
    stmt->params_count = (uint16_t)binary_get_uinteger(breader, 2, 1);
    if (stmt->field_count > 0) {
        mysql->parse_status = STMT_PREPARE_FIELD;
        CALLOC(stmt->fields, 1, sizeof(mpack_field) * (size_t)stmt->field_count);
    }
    if (stmt->params_count > 0) {
        mysql->parse_status = STMT_PREPARE_PARAMS;
        CALLOC(stmt->params, 1, sizeof(mpack_field) * (size_t)stmt->params_count);
    }
    mysql->mpack->pack = stmt;
    mysql->mpack->_free_mpack = _mpack_stm_free;
}
static mpack_ctx *_prepare_response(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    if (0 == mysql->parse_status) {
        if (MYSQL_ERR == (uint8_t)(binary_at(breader, 0)[0])) {
            binary_get_skip(breader, 1);
            mpack_ctx * mpack = _mpack_new(mysql, breader->data);
            mpack->pack_type = MPACK_ERR;
            MALLOC(mpack->pack, sizeof(mpack_err));
            _mpack_err(mysql, breader, mpack->pack);
            mysql->cur_cmd = 0;
            return mpack;
        }
        binary_get_skip(breader, 1);
        _mpack_stmt_new(mysql, breader);
        FREE(breader->data);
        if (0 == mysql->parse_status) {
            mpack_ctx *mpack = mysql->mpack;
            mysql->mpack = NULL;
            mysql->cur_cmd = 0;
            return mpack;
        }
        if (ERR_OK != _mpack_more_data(mysql, buf, breader, status)) {
            return NULL;
        }
        return _mpack_stmt(mysql, buf, breader, status);
    } else {
        return _mpack_stmt(mysql, buf, breader, status);
    }
}
static mpack_ctx *_execute_response(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    mpack_ctx *mpack = NULL;
    if (0 == mysql->parse_status) {
        switch ((uint8_t)(binary_at(breader, 0)[0])) {
        case MYSQL_OK:
            binary_get_skip(breader, 1);
            mpack = _mpack_new(mysql, breader->data);
            mpack->pack_type = MPACK_OK;
            MALLOC(mpack->pack, sizeof(mpack_ok));
            _mpack_ok(mysql, breader, mpack->pack);
            mysql->cur_cmd = 0;
            break;
        case MYSQL_ERR:
            binary_get_skip(breader, 1);
            mpack = _mpack_new(mysql, breader->data);
            mpack->pack_type = MPACK_ERR;
            MALLOC(mpack->pack, sizeof(mpack_err));
            _mpack_err(mysql, breader, mpack->pack);
            mysql->cur_cmd = 0;
            break;
        default:
            _mpack_reader_new(mysql, breader, MPACK_STMT_EXECUTE);//读取字段数
            FREE(breader->data);
            mysql->parse_status = RST_FIELD;
            if (ERR_OK != _mpack_more_data(mysql, buf, breader, status)) {
                break;
            }
            mpack = _mpack_reader(mysql, buf, breader, status);
            break;
        }
    } else {
        mpack = _mpack_reader(mysql, buf, breader, status);
    }
    return mpack;
}
static mpack_ctx *_reset_response(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    mpack_ctx *mpack = _mpack_new(mysql, breader->data);
    if (MYSQL_OK == binary_get_uint8(breader)) {
        mpack->pack_type = MPACK_OK;
        MALLOC(mpack->pack, sizeof(mpack_ok));
        _mpack_ok(mysql, breader, mpack->pack);
    } else {
        mpack->pack_type = MPACK_ERR;
        MALLOC(mpack->pack, sizeof(mpack_err));
        _mpack_err(mysql, breader, mpack->pack);
    }
    mysql->cur_cmd = 0;
    return mpack;
}
mpack_ctx *_mpack_parser(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    mpack_ctx *mpack = NULL;
    switch (mysql->cur_cmd) {
    case MYSQL_QUIT:
        FREE(breader->data);
        BIT_SET(*status, PROT_CLOSE);
        break;
    case MYSQL_INIT_DB:
        mpack = _selectdb_response(mysql, breader);
        break;
    case MYSQL_PING:
        mpack = _ping_response(mysql, breader);
        break;
    case MYSQL_QUERY:
        mpack = _query_response(mysql, buf, breader, status);
        break;
    case MYSQL_PREPARE:
        mpack = _prepare_response(mysql, buf, breader, status);
        break;
    case MYSQL_EXECUTE:
        mpack = _execute_response(mysql, buf, breader, status);
        break;
    case MYSQL_STMT_RESET:
        mpack = _reset_response(mysql, buf, breader, status);
        break;
    default:
        FREE(breader->data);
        BIT_SET(*status, PROT_ERROR);
        break;
    }
    return mpack;
}
