#include "proto/mysql_parse.h"
#include "proto/protos.h"
#include "utils.h"

typedef enum QUERY_STATUS {
    QUERY_FIELD = 0x01,
    QUERY_ROW
}QUERY_STATUS;

static uint64_t _mysql_get_lenenc(binary_ctx *breader) {
    uint8_t flag = binary_get_uint8(breader);
    if (flag <= 0xfa) {
        return flag;
    }
    if (0xfc == flag) {
        return (uint64_t)binary_get_uint16(breader, 2, 1);
    }
    if (0xfd == flag) {
        return (uint64_t)binary_get_uint32(breader, 3, 1);
    }
    if (0xfe == flag) {
        return binary_get_uint64(breader, 8, 1);
    }
    LOG_ERROR("unknow int<lenenc>, %d.", (int32_t)flag);
    return 0;
}
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
        BIT_SET(*status, PROTO_MOREDATA);
        return NULL;
    }
    char *payload;
    MALLOC(payload, *payload_lens);
    ASSERTAB(*payload_lens == buffer_remove(buf, payload, *payload_lens), "copy buffer failed.");
    return payload;
}
void _mpack_ok(binary_ctx *breader, mpack_ok *ok) {
    uint64_t size = _mysql_get_lenenc(breader);
    ok->affected_rows = (int64_t)size;
    size = _mysql_get_lenenc(breader);
    ok->last_insert_id = (int64_t)size;
    ok->status_flags = binary_get_int16(breader, 2, 1);
    ok->warnings = binary_get_int16(breader, 2, 1);
    binary_get_skip(breader, breader->size - breader->offset);
}
void _mpack_eof(binary_ctx *breader, mpack_eof *eof) {
    eof->warnings = binary_get_int16(breader, 2, 1);
    eof->status_flags = binary_get_int16(breader, 2, 1);
}
void _mpack_err(binary_ctx *breader, mpack_err *err) {
    err->error_code = binary_get_int16(breader, 2, 1);
    binary_get_skip(breader, 6);//sql_state_marker sql_state
    err->error_msg.lens = breader->size - breader->offset;
    err->error_msg.data = binary_get_string(breader, err->error_msg.lens);
}
static mpack_ctx *_mysql_mpack_new(mysql_ctx *mysql, char *payload) {
    mpack_ctx *mpack;
    CALLOC(mpack, 1, sizeof(mpack_ctx));
    mpack->sequence_id = mysql->id;
    mpack->payload = payload;
    return mpack;
}
static mpack_ctx *_mysql_selectdb_response(mysql_ctx *mysql, binary_ctx *breader) {
    mpack_ctx *mpack = _mysql_mpack_new(mysql, breader->data);
    if (MYSQL_OK == binary_get_uint8(breader)) {
        mpack->pack_type = MPACK_OK;
        MALLOC(mpack->pack, sizeof(mpack_ok));
        _mpack_ok(breader, mpack->pack);
    } else {
        mpack->pack_type = MPACK_ERR;
        MALLOC(mpack->pack, sizeof(mpack_err));
        _mpack_err(breader, mpack->pack);
    }
    mysql->cur_cmd = 0;
    return mpack;
}
static mpack_ctx *_mysql_ping_response(mysql_ctx *mysql, binary_ctx *breader) {
    mpack_ctx *mpack = _mysql_mpack_new(mysql, breader->data);
    binary_get_skip(breader, 1);
    mpack->pack_type = MPACK_OK;
    MALLOC(mpack->pack, sizeof(mpack_ok));
    _mpack_ok(breader, mpack->pack);
    mysql->cur_cmd = 0;
    return mpack;
}
static void _mpack_query_free(void *pack) {
    mpack_row *rows;
    mpack_query *query = pack;
    for (uint32_t i = 0; i < arr_ptr_size(&query->arr_rows); i++) {
        rows = *(mpack_row **)(arr_ptr_at(&query->arr_rows, i));
        FREE(rows->payload);
        FREE(rows);
    }
    arr_ptr_free(&query->arr_rows);
    FREE(query->fields);
}
static void _mpack_query_pack_init(mysql_ctx *mysql, binary_ctx *breader) {
    mysql->mpack = _mysql_mpack_new(mysql, NULL);
    mpack_query *query;
    CALLOC(query, 1, sizeof(mpack_query));
    query->field_count = (int32_t)_mysql_get_lenenc(breader);//column_count
    CALLOC(query->fields, 1, sizeof(mpack_field) * (size_t)query->field_count);
    arr_ptr_init(&query->arr_rows, ONEK);
    mysql->mpack->pack = query;
    mysql->mpack->_free_mpack = _mpack_query_free;
    mysql->mpack->pack_type = MPACK_QUERY;
}
static int32_t _mysql_more_data(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    size_t payload_lens;
    char *payload = _mysql_payload(mysql, buf, &payload_lens, status);
    if (NULL == payload) {
        return ERR_FAILED;
    }
    binary_init(breader, payload, payload_lens, 0);
    return ERR_OK;
}
static int32_t _mysql_check_final(binary_ctx *breader, int32_t *status) {
    binary_get_skip(breader, 1);
    mpack_eof eof;
    _mpack_eof(breader, &eof);
    if (BIT_CHECK(eof.status_flags, SERVER_MORE_RESULTS_EXISTS)) {
        BIT_SET(*status, PROTO_MOREDATA);
        return ERR_FAILED;
    }
    return ERR_OK;
}
static void _mysql_parse_query_row(mpack_query *query, binary_ctx *breader) {
    mpack_row *row;
    CALLOC(row, 1, sizeof(mpack_row) * (size_t)query->field_count);
    row->payload = breader->data;
    for (int32_t i = 0; i < query->field_count; i++) {
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
    arr_ptr_push_back(&query->arr_rows, (void **)&row);
}
static mpack_ctx *_mysql_query_row(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    mpack_query *query = mysql->mpack->pack;
    for (;;) {
        if (MYSQL_EOF == (uint8_t)(binary_at(breader, breader->offset)[0])) {
            if (ERR_OK != _mysql_check_final(breader, status)) {
                FREE(breader->data);
                return NULL;
            }
            FREE(breader->data);
            mpack_ctx *mpack = mysql->mpack; //row解析完成
            mysql->mpack = NULL;
            mysql->cur_cmd = 0;
            return mpack;
        }
        _mysql_parse_query_row(query, breader);
        if (ERR_OK != _mysql_more_data(mysql, buf, breader, status)) {
            return NULL;
        }
    }
    return NULL;
}
static void _mysql_parse_field(binary_ctx *breader, mpack_field *field) {
    uint64_t lens = _mysql_get_lenenc(breader);
    binary_get_skip(breader, (size_t)lens);//catalog
    lens = _mysql_get_lenenc(breader);
    char *val = binary_get_string(breader, (size_t)lens);
    memcpy(field->schema, val, (size_t)lens);
    lens = _mysql_get_lenenc(breader);
    val = binary_get_string(breader, (size_t)lens);
    memcpy(field->table, val, (size_t)lens);
    lens = _mysql_get_lenenc(breader);
    val = binary_get_string(breader, (size_t)lens);
    memcpy(field->org_table, val, (size_t)lens);
    lens = _mysql_get_lenenc(breader);
    val = binary_get_string(breader, (size_t)lens);
    memcpy(field->name, val, (size_t)lens);
    lens = _mysql_get_lenenc(breader);
    val = binary_get_string(breader, (size_t)lens);
    memcpy(field->org_name, val, (size_t)lens);
    lens = _mysql_get_lenenc(breader);//length of fixed length fields
    field->character = binary_get_int16(breader, 2, 1);
    field->field_lens = binary_get_int32(breader, 4, 1);
    field->type = binary_get_uint8(breader);
    field->flags = binary_get_int16(breader, 2, 1);
    field->decimals = binary_get_uint8(breader);
}
static mpack_ctx *_mysql_query_filed(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    mpack_query *query = mysql->mpack->pack;
    for (;;) {
        if (MYSQL_EOF == (uint8_t)(binary_at(breader, breader->offset)[0])) {
            if (ERR_OK != _mysql_check_final(breader, status)) {
                FREE(breader->data);
                return NULL;
            }
            FREE(breader->data);
            mysql->parse_status = QUERY_ROW;//filed解析完成
            break;
        }
        _mysql_parse_field(breader, &query->fields[query->index]);
        ++query->index;
        FREE(breader->data);
        if (ERR_OK != _mysql_more_data(mysql, buf, breader, status)) {
            return NULL;
        }
    }
    if (ERR_OK != _mysql_more_data(mysql, buf, breader, status)) {
        return NULL;
    }
    return _mysql_query_row(mysql, buf, breader, status);
}
static mpack_ctx *_mysql_query_results(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    mpack_ctx *mpack = NULL;
    switch (mysql->parse_status) {
    case 0: {
        _mpack_query_pack_init(mysql, breader);//读取字段数
        FREE(breader->data);
        mysql->parse_status = QUERY_FIELD;
        if (ERR_OK != _mysql_more_data(mysql, buf, breader, status)) {
            break;
        }
        mpack = _mysql_query_filed(mysql, buf, breader, status);
        break;
    }
    case QUERY_FIELD:
        mpack = _mysql_query_filed(mysql, buf, breader, status);
        break;
    case QUERY_ROW:
        mpack = _mysql_query_row(mysql, buf, breader, status);
        break;
    default:
        break;
    }
    return mpack;
}
static mpack_ctx *_mysql_query_response(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    mpack_ctx *mpack = NULL;
    if (0 == mysql->parse_status) {
        switch ((uint8_t)(binary_at(breader, 0)[0])) {
        case MYSQL_OK:
            binary_get_skip(breader, 1);
            mpack = _mysql_mpack_new(mysql, breader->data);
            mpack->pack_type = MPACK_OK;
            MALLOC(mpack->pack, sizeof(mpack_ok));
            _mpack_ok(breader, mpack->pack);
            mysql->cur_cmd = 0;
            break;
        case MYSQL_ERR:
            binary_get_skip(breader, 1);
            mpack = _mysql_mpack_new(mysql, breader->data);
            mpack->pack_type = MPACK_ERR;
            MALLOC(mpack->pack, sizeof(mpack_err));
            _mpack_err(breader, mpack->pack);
            mysql->cur_cmd = 0;
            break;
        case MYSQL_LOCAL_INFILE:
            BIT_SET(*status, PROTO_ERROR);
            FREE(breader->data);
            break;
        default: {
            mpack = _mysql_query_results(mysql, buf, breader, status);
            break;
        }
        }
    } else {
        mpack = _mysql_query_results(mysql, buf, breader, status);
    }
    return mpack;
}
mpack_ctx *_mpack_parser(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    mpack_ctx *mpack = NULL;
    switch (mysql->cur_cmd) {
    case MYSQL_QUIT:
        BIT_SET(*status, PROTO_ERROR);
        break;
    case MYSQL_INIT_DB:
        mpack = _mysql_selectdb_response(mysql, breader);
        break;
    case MYSQL_PING:
        mpack = _mysql_ping_response(mysql, breader);
        break;
    case MYSQL_QUERY:
        mpack = _mysql_query_response(mysql, buf, breader, status);
        break;
    default:
        break;
    }
    return mpack;
}
