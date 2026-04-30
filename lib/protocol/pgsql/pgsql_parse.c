#include "protocol/pgsql/pgsql_parse.h"

// 解析 ErrorResponse / NoticeResponse，将各字段拼接为可读字符串返回（调用方负责释放）
char *_pgpack_error_notice(binary_ctx *breader) {
    char flag;
    char *tmp;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    for (;;) {
        flag = binary_get_int8(breader);   // 字段类型标志（如 'S'=严重性, 'M'=消息等）
        tmp = binary_get_string(breader, 0);
        binary_set_int8(&bwriter, flag);
        binary_set_string(&bwriter, ": ", 2);
        if (breader->size - breader->offset > 1) { // 还有后续字段（1 字节为结束标志）
            binary_set_va(&bwriter, "%s\r\n", tmp);
        } else {
            binary_set_string(&bwriter, tmp, 0); // 最后一个字段，不追加换行
            break;
        }
    }
    return bwriter.data;
}
// 分配并初始化一个空的 pgpack_ctx
static pgpack_ctx *_pgpack_new(pgpack_type type) {
    pgpack_ctx *pgpack;
    CALLOC(pgpack, 1, sizeof(pgpack_ctx));
    pgpack->type = type;
    return pgpack;
}
// 获取或创建 pgsql_ctx 当前累积的 pgpack_ctx；pg 为 NULL 时直接分配新的（用于通知包）
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
        pgpack->_free_pgpack(pgpack->pack); // 释放内部数据（reader 或 notification）
    }
    FREE(pgpack->pack);
    FREE(pgpack);
}
// 释放 pgpack_notification 持有的原始消息缓冲区
static void _pgpack_notification_response_free(void *arg) {
    pgpack_notification *notification = arg;
    FREE(notification->payload);
}
// 解析 NotificationResponse（'A'），返回新分配的 pgpack_ctx（PGPACK_NOTIFICATION 类型）
static pgpack_ctx *_pgpack_notification_response(binary_ctx *breader) {
    pgpack_notification *notification;
    MALLOC(notification, sizeof(pgpack_notification));
    notification->payload = breader->data; // 接管原始消息缓冲区的所有权
    notification->pid = (int32_t)binary_get_integer(breader, 4, 0);
    notification->channel = binary_get_string(breader, 0);
    notification->notification = binary_get_string(breader, 0);
    pgpack_ctx *pgpack = _pgpack_init(NULL, PGPACK_NOTIFICATION);
    pgpack->pack = notification;
    pgpack->_free_pgpack = _pgpack_notification_response_free;
    return pgpack;
}
// 释放 pgsql_reader_ctx 内部所有行数据和字段描述（不释放结构体本身）
void _pgpack_reader_free(void *arg) {
    pgsql_reader_ctx *reader = arg;
    pgpack_row *row;
    for (uint32_t i = 0; i < arr_ptr_size(&reader->arr_rows); i++) {
        row = *arr_ptr_at(&reader->arr_rows, i);
        FREE(row->payload); // 释放首列持有的原始行缓冲区
        FREE(row);
    }
    arr_ptr_free(&reader->arr_rows);
    FREE(reader->fields);
}
// 获取或创建 pgpack_ctx 中的 pgsql_reader_ctx，并设置释放回调
static pgsql_reader_ctx *_pgpack_reader_init(pgpack_ctx *pgpack) {
    if (NULL != pgpack->pack) {
        return pgpack->pack; // 已存在则复用（同一查询的多条 DataRow 共享同一 reader）
    }
    pgsql_reader_ctx *reader;
    CALLOC(reader, 1, sizeof(pgsql_reader_ctx));
    pgpack->pack = reader;
    pgpack->_free_pgpack = _pgpack_reader_free;
    arr_ptr_init(&reader->arr_rows, 0);
    return reader;
}
// 解析 RowDescription（'T'），填充字段描述数组
static void _pgpack_row_description(pgpack_ctx *pgpack, binary_ctx *breader) {
    pgsql_reader_ctx *reader = _pgpack_reader_init(pgpack);
    reader->field_count = (int16_t)binary_get_integer(breader, 2, 0);
    if (reader->field_count <= 0) {
        return;
    }
    MALLOC(reader->fields, sizeof(pgpack_field) * (size_t)reader->field_count);
    char *fname;
    pgpack_field *field;
    for (int16_t i = 0; i < reader->field_count; i++) {
        field = &reader->fields[i];
        fname = binary_get_string(breader, 0);
        strncpy(field->name, fname, sizeof(field->name) - 1);
        field->name[sizeof(field->name) - 1] = '\0';
        field->table_oid = (int32_t)binary_get_integer(breader, 4, 0);
        field->index = (int16_t)binary_get_integer(breader, 2, 0);
        field->type_oid = (int32_t)binary_get_integer(breader, 4, 0);
        field->lens = (int16_t)binary_get_integer(breader, 2, 0);
        field->type_modifier = (int32_t)binary_get_integer(breader, 4, 0);
        field->format = (pgpack_format)binary_get_integer(breader, 2, 0);
    }
}
// 解析 DataRow（'D'），将列值追加到 reader 的行数组中（接管 breader->data 所有权）
static void _pgpack_data_row(pgpack_ctx *pgpack, binary_ctx *breader) {
    int16_t ncolumn = (int16_t)binary_get_integer(breader, 2, 0);
    if (ncolumn <= 0) {
        FREE(breader->data);
        return;
    }
    pgsql_reader_ctx *reader = _pgpack_reader_init(pgpack);
    if (0 == reader->field_count) {
        reader->field_count = ncolumn; // 没有 RowDescription 时从 DataRow 推断列数
    }
    pgpack_row *row;
    pgpack_row *rows;
    MALLOC(rows, sizeof(pgpack_row) * (size_t)ncolumn);
    rows->payload = breader->data; // 首列持有原始消息缓冲区所有权
    for (int16_t i = 0; i < ncolumn; i++) {
        row = &rows[i];
        row->lens = (int32_t)binary_get_integer(breader, 4, 0);
        if (row->lens > 0) {
            row->val = breader->data + breader->offset; // 列值直接指向缓冲区内部
            binary_get_skip(breader, row->lens);
        } else {
            row->val = NULL; // lens 为 -1 表示 NULL，0 表示空字符串
        }
    }
    arr_ptr_push_back(&reader->arr_rows, (void **)&rows);
}
// 解析一个完整的服务端消息，在收到 ReadyForQuery 时返回已累积的 pgpack_ctx
pgpack_ctx *_pgpack_parser(pgsql_ctx *pg, binary_ctx *breader, ud_cxt *ud, int32_t *status) {
    (void)ud;
    (void)status;
    pgpack_ctx *pack = NULL;
    int8_t code = binary_get_int8(breader);  // 读取消息类型码
    binary_get_skip(breader, 4);             // 跳过消息体长度字段
    switch (code) { // N / S / A 随时都有可能收到（异步消息）
    case 'N': // NoticeResponse：服务端通知消息，忽略
        FREE(breader->data);
        break;
    case 'S': // ParameterStatus：运行时参数状态报告，忽略
        FREE(breader->data);
        break;
    case 'A': // NotificationResponse：LISTEN 产生的异步通知，立即返回给上层
        pack = _pgpack_notification_response(breader);
        break;
    case 'E': // ErrorResponse：命令执行出错
        if (NULL != pg->pack) {
            _pgpack_free(pg->pack); // 丢弃此前累积的部分结果
            pg->pack = NULL;
            LOG_WARN("an error occurred during the query.");
        }
        _pgpack_init(pg, PGPACK_ERR);
        pg->pack->pack = _pgpack_error_notice(breader); // 保存错误描述字符串
        FREE(breader->data);
        break;
    case 'n': // NoData：Describe 结果为空（无行描述），忽略
        FREE(breader->data);
        break;
    case 'I': // EmptyQueryResponse：Query 收到空 SQL，标记为 OK
        _pgpack_init(pg, PGPACK_OK);
        FREE(breader->data);
        break;
    case '1': // ParseComplete：Parse 命令完成
        _pgpack_init(pg, PGPACK_OK);
        FREE(breader->data);
        break;
    case '2': // BindComplete：Bind 命令完成
        _pgpack_init(pg, PGPACK_OK);
        FREE(breader->data);
        break;
    case '3': // CloseComplete：Close 命令完成
        _pgpack_init(pg, PGPACK_OK);
        FREE(breader->data);
        break;
    case 't': // ParameterDescription：Describe 返回的参数类型描述，当前忽略
        FREE(breader->data);
        break;
    case 'T': // RowDescription：行描述，初始化 reader 并填充字段信息
        _pgpack_init(pg, PGPACK_OK);
        _pgpack_row_description(pg->pack, breader);
        FREE(breader->data);
        break;
    case 'D': // DataRow：数据行，追加到 reader（注意：不释放 breader->data，所有权转移）
        _pgpack_init(pg, PGPACK_OK);
        _pgpack_data_row(pg->pack, breader);
        break;
    case 'C': { // CommandComplete：命令完成，记录命令标签
        _pgpack_init(pg, PGPACK_OK);
        char *complete = binary_get_string(breader, 0);
        if (!EMPTYSTR(complete)) {
            strncpy(pg->pack->complete, complete, sizeof(pg->pack->complete) - 1);
            pg->pack->complete[sizeof(pg->pack->complete) - 1] = '\0';
        }
        FREE(breader->data);
        break;
    }
    case 'Z': // ReadyForQuery：服务端就绪，将累积的结果包返回给调用方
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
