#include "protocol/pgsql/pgsql_parse.h"

//ErrorResponse NoticeResponse
char *_pgpack_error_notice(binary_ctx *breader) {
    char flag;
    char *tmp;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    for (;;) {
        flag = binary_get_int8(breader);
        tmp = binary_get_string(breader, 0);
        binary_set_int8(&bwriter, flag);
        binary_set_string(&bwriter, ": ", 2);
        if (breader->size - breader->offset > 1) {//1 OPCODE
            binary_set_va(&bwriter, "%s\r\n", tmp);
        } else {
            binary_set_string(&bwriter, tmp, 0);
            break;
        }
    }
    return bwriter.data;
}
static pgpack_ctx *_pgpack_new(pgpack_type type) {
    pgpack_ctx *pgpack;
    CALLOC(pgpack, 1, sizeof(pgpack_ctx));
    pgpack->type = type;
    return pgpack;
}
static pgpack_ctx *_pgpack_init(pgsql_ctx *pg, pgpack_type type) {
    if (NULL == pg) {
        return _pgpack_new(type);
    }
    if (NULL == pg->pack) {
        pg->pack = _pgpack_new(type);
        return pg->pack;
    }
    pgpack_type oldtype = ((pgpack_ctx *)pg->pack)->type;
    if (type != oldtype) {
        LOG_WARN("different pack type: %d  %d.", oldtype, type);
    }
    return pg->pack;
}
void _pgpack_free(pgpack_ctx *pgpack) {
    if (NULL == pgpack) {
        return;
    }
    if (NULL != pgpack->_free_pgpack) {
        pgpack->_free_pgpack(pgpack->pack);
    }
    FREE(pgpack->pack);
    FREE(pgpack->complete);
    FREE(pgpack);
}
//NotificationResponse
static void _pgpack_notification_response_free(void *arg) {
    pgpack_notification *notification = arg;
    FREE(notification->payload);
}
static pgpack_ctx *_pgpack_notification_response(binary_ctx *breader) {
    pgpack_notification *notification;
    MALLOC(notification, sizeof(pgpack_notification));
    notification->payload = breader->data;
    notification->pid = (int32_t)binary_get_integer(breader, 4, 0);
    notification->channel = binary_get_string(breader, 0);
    notification->notification = binary_get_string(breader, 0);
    pgpack_ctx *pgpack = _pgpack_init(NULL, PGPACK_NOTIFICATION);
    pgpack->pack = notification;
    pgpack->_free_pgpack = _pgpack_notification_response_free;
    return pgpack;
}
//RowDescription
void _pgpack_reader_free(void *arg) {
    pgsql_reader_ctx *reader = arg;
    pgpack_row *row;
    for (uint32_t i = 0; i < arr_ptr_size(&reader->arr_rows); i++) {
        row = *arr_ptr_at(&reader->arr_rows, i);
        FREE(row->payload);
        FREE(row);
    }
    arr_ptr_free(&reader->arr_rows);
    FREE(reader->fields);
}
static pgsql_reader_ctx *_pgpack_reader_init(pgpack_ctx *pgpack) {
    if (NULL != pgpack->pack) {
        return pgpack->pack;
    }
    pgsql_reader_ctx *reader;
    CALLOC(reader, 1, sizeof(pgsql_reader_ctx));
    pgpack->pack = reader;
    pgpack->_free_pgpack = _pgpack_reader_free;
    arr_ptr_init(&reader->arr_rows, 0);
    return reader;
}
static void _pgpack_row_description(pgpack_ctx *pgpack, binary_ctx *breader) {
    pgsql_reader_ctx *reader = _pgpack_reader_init(pgpack);
    reader->field_count = (int16_t)binary_get_integer(breader, 2, 0);
    if (0 == reader->field_count) {
        return;
    }
    MALLOC(reader->fields, sizeof(pgpack_field) * reader->field_count);
    pgpack_field *field;
    for (int16_t i = 0; i < reader->field_count; i++) {
        field = &reader->fields[i];
        strcpy(field->name, binary_get_string(breader, 0));
        field->table_oid = (int32_t)binary_get_integer(breader, 4, 0);
        field->index = (int16_t)binary_get_integer(breader, 2, 0);
        field->type_oid = (int32_t)binary_get_integer(breader, 4, 0);
        field->lens = (int16_t)binary_get_integer(breader, 2, 0);
        field->type_modifier = (int32_t)binary_get_integer(breader, 4, 0);
        field->format = (pgpack_format)binary_get_integer(breader, 2, 0);
    }
}
//DataRow
static void _pgpack_data_row(pgpack_ctx *pgpack, binary_ctx *breader) {
    int16_t ncolumn = (int16_t)binary_get_integer(breader, 2, 0);
    if (0 == ncolumn) {
        FREE(breader->data);
        return;
    }
    pgsql_reader_ctx *reader = _pgpack_reader_init(pgpack);
    if (0 == reader->field_count) {
        reader->field_count = ncolumn;
    }
    pgpack_row *row;
    pgpack_row *rows;
    MALLOC(rows, sizeof(pgpack_row) * ncolumn);
    rows->payload = breader->data;
    for (int16_t i = 0; i < ncolumn; i++) {
        row = &rows[i];
        row->lens = (int32_t)binary_get_integer(breader, 4, 0);
        if (row->lens > 0) {
            row->val = breader->data + breader->offset;
            binary_get_skip(breader, row->lens);
        } else {
            row->val = NULL;
        }
    }
    arr_ptr_push_back(&reader->arr_rows, (void **)&rows);
}
pgpack_ctx *_pgpack_parser(pgsql_ctx *pg, binary_ctx *breader, ud_cxt *ud, int32_t *status) {
    pgpack_ctx *pack = NULL;
    int8_t code = binary_get_int8(breader);
    binary_get_skip(breader, 4);
    switch (code) {//N S A 随时都有可能收到
    case 'N'://NoticeResponse
        FREE(breader->data);
        break;
    case 'S'://ParameterStatus 运行时参数状态报告
        FREE(breader->data);
        break;
    case 'A'://NotificationResponse  LISTEN命令(可以用来进行多应用间的通信)
        pack = _pgpack_notification_response(breader);
        break;

    case 'E'://ErrorResponse  错误
        if (NULL != pg->pack) {
            _pgpack_free(pg->pack);
            pg->pack = NULL;
            LOG_WARN("an error occurred during the query.");
        }
        _pgpack_init(pg, PGPACK_ERR);
        pg->pack->pack = _pgpack_error_notice(breader);
        FREE(breader->data);
        break;
    case 'n'://NoData 
        FREE(breader->data);
        break;
    case 'I'://EmptyQueryResponse Query
        _pgpack_init(pg, PGPACK_OK);
        FREE(breader->data);
        break;
    case '1'://ParseComplete Parse
        _pgpack_init(pg, PGPACK_OK);
        FREE(breader->data);
        break;
    case '2'://BindComplete Bind
        _pgpack_init(pg, PGPACK_OK);
        FREE(breader->data);
        break;
    case '3'://CloseComplete Close
        _pgpack_init(pg, PGPACK_OK);
        FREE(breader->data);
        break;
    case 't'://ParameterDescription Describe
        FREE(breader->data);
        break;
    case 'T'://RowDescription
        _pgpack_init(pg, PGPACK_OK);
        _pgpack_row_description(pg->pack, breader);
        FREE(breader->data);
        break;
    case 'D'://DataRow 
        _pgpack_init(pg, PGPACK_OK);
        _pgpack_data_row(pg->pack, breader);
        break;
    case 'C': { //CommandComplete
        _pgpack_init(pg, PGPACK_OK);
        char *complete = binary_get_string(breader, 0);
        if (!EMPTYSTR(complete)) {
            MALLOC(pg->pack->complete, strlen(complete) + 1);
            strcpy(pg->pack->complete, complete);
        }
        FREE(breader->data);
        break;
    }
    case 'Z'://ReadyForQuery
        pg->readyforquery = binary_get_int8(breader);
        pack = pg->pack;
        pg->pack = NULL;
        FREE(breader->data);
        break;
    default:
        LOG_WARN("unknown opcode %c.", code);
        FREE(breader->data);
        break;
    }
    return pack;
}
