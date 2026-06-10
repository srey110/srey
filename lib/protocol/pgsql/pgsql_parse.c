#include "protocol/pgsql/pgsql_parse.h"
#include "utils/utils.h"

// 解析 ErrorResponse / NoticeResponse，将各字段拼接为可读字符串返回（调用方负责释放）
char *_pgpack_error_notice(binary_ctx *breader) {
    char flag;
    char *tmp;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    for (;;) {
        if (breader->size - breader->offset < 1) {
            break;
        }
        flag = binary_get_int8(breader);   // 字段类型标志（如 'S'=严重性, 'M'=消息等）
        if (0 == flag) {
            break;
        }
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
    binary_set_int8(&bwriter, 0);
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
    // 类型不符: 释放旧内部 pack 数据,保留外壳与 complete 字段
    if (type != oldtype) {
        if (NULL != pg->pack->_free_pgpack) {
            pg->pack->_free_pgpack(pg->pack->pack);
        }
        FREE(pg->pack->pack);
        pg->pack->_free_pgpack = NULL;
        pg->pack->type = type;
        LOG_WARN("different pack type: %d  %d, discard previous.", oldtype, type);
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
// 释放 pgpack_copy_out_ctx 内部累积的数据缓冲区
static void _pgpack_copy_out_free(void *arg) {
    pgpack_copy_out_ctx *copyout = arg;
    FREE(copyout->data.data);
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
    for (uint32_t i = 0; i < array_size(&reader->arr_rows); i++) {
        row = *(pgpack_row **)array_at(&reader->arr_rows, i);
        FREE(row->payload); // 释放首列持有的原始行缓冲区
        FREE(row);
    }
    array_free(&reader->arr_rows);
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
    array_init(&reader->arr_rows, sizeof(pgpack_row *), 0);
    return reader;
}
// 解析 RowDescription（'T'），填充字段描述数组
static int32_t _pgpack_row_description(pgpack_ctx *pgpack, binary_ctx *breader) {
    pgsql_reader_ctx *reader = _pgpack_reader_init(pgpack);
    // 不支持多语句 simple query
    if (NULL != reader->fields) {
        LOG_WARN("multi-statement simple query not supported (received second RowDescription).");
        return ERR_FAILED;
    }
    reader->field_count = (uint16_t)binary_get_uinteger(breader, 2, 0);
    if (0 == reader->field_count) {
        return ERR_OK;
    }
    size_t remaining = breader->size - breader->offset;
    if ((size_t)reader->field_count * 19 > remaining) {
        reader->field_count = 0;
        return ERR_FAILED;
    }
    MALLOC(reader->fields, sizeof(pgpack_field) * (size_t)reader->field_count);
    char *fname;
    pgpack_field *field;
    for (uint16_t i = 0; i < reader->field_count; i++) {
        field = &reader->fields[i];
        fname = binary_get_string(breader, 0);
        safe_fill_str(field->name, sizeof(field->name), fname);
        field->table_oid = (int32_t)binary_get_integer(breader, 4, 0);
        field->index = (int16_t)binary_get_integer(breader, 2, 0);
        field->type_oid = (int32_t)binary_get_integer(breader, 4, 0);
        field->lens = (int16_t)binary_get_integer(breader, 2, 0);
        field->type_modifier = (int32_t)binary_get_integer(breader, 4, 0);
        field->format = (pgpack_format)binary_get_integer(breader, 2, 0);
    }
    return ERR_OK;
}
// 解析 DataRow（'D'），将列值追加到 reader 的行数组中（成功时接管 breader->data 所有权）
// 返回 ERR_OK 表示成功（breader->data 已转交 rows[0].payload，由 reader 释放）
// 返回 ERR_FAILED 表示协议异常（breader->data 已被释放，调用方不可再触碰）
static int32_t _pgpack_data_row(pgpack_ctx *pgpack, binary_ctx *breader) {
    uint16_t ncolumn = (uint16_t)binary_get_uinteger(breader, 2, 0);
    if (0 == ncolumn) {
        FREE(breader->data);
        return ERR_OK;
    }
    pgsql_reader_ctx *reader = _pgpack_reader_init(pgpack);
    if (NULL == reader->fields || ncolumn != reader->field_count) {
        FREE(breader->data);
        return ERR_FAILED;
    }
    pgpack_row *row;
    pgpack_row *rows;
    CALLOC(rows, ncolumn, sizeof(pgpack_row));
    rows->payload = breader->data; // 首列持有原始消息缓冲区所有权
    for (uint16_t i = 0; i < ncolumn; i++) {
        row = &rows[i];
        row->lens = (int32_t)binary_get_integer(breader, 4, 0);
        if (row->lens > 0) {
            if ((size_t)row->lens > breader->size - breader->offset) {
                FREE(rows);
                FREE(breader->data);
                return ERR_FAILED;
            }
            row->val = breader->data + breader->offset;
            binary_get_skip(breader, row->lens);
        } else if (0 == row->lens) {
            row->val = breader->data + breader->offset; // 空字符串：有效地址，长度为 0
        } else if (-1 == row->lens) {
            row->val = NULL; // SQL NULL
        } else {
            // 非法 column length（PostgreSQL 协议仅 -1 表 NULL，其他负数协议非法）
            FREE(rows);
            FREE(breader->data);
            return ERR_FAILED;
        }
    }
    array_push_back(&reader->arr_rows, &rows);
    return ERR_OK;
}
// 解析 CopyInResponse（'G'），返回新分配的 pgpack_ctx（PGPACK_COPY_IN 类型，立即返回给调用方）
static pgpack_ctx *_pgpack_copy_in_response(binary_ctx *breader) {
    pgpack_copy_in_ctx *copyin;
    MALLOC(copyin, sizeof(pgpack_copy_in_ctx));
    copyin->format = (pgpack_format)binary_get_int8(breader);
    copyin->ncol = (int16_t)binary_get_integer(breader, 2, 0);
    pgpack_ctx *pgpack = _pgpack_init(NULL, PGPACK_COPY_IN);
    pgpack->pack = copyin;
    return pgpack;
}
// 解析 CopyOutResponse（'H'），初始化 pg->pack 中的 PGPACK_COPY_OUT 累积缓冲区
static void _pgpack_copy_out_response(pgpack_ctx *pgpack, binary_ctx *breader) {
    // 防御非法序列（如 'H'→'H'）：覆写前先释放可能残留的旧 pack
    if (NULL != pgpack->pack) {
        if (NULL != pgpack->_free_pgpack) {
            pgpack->_free_pgpack(pgpack->pack);
        }
        FREE(pgpack->pack);
    }
    pgpack_copy_out_ctx *copyout;
    CALLOC(copyout, 1, sizeof(pgpack_copy_out_ctx));
    copyout->format = (pgpack_format)binary_get_int8(breader);
    copyout->ncol = (int16_t)binary_get_integer(breader, 2, 0);
    binary_init(&copyout->data, NULL, 0, 0);
    pgpack->pack = copyout;
    pgpack->_free_pgpack = _pgpack_copy_out_free;
}
// 解析 CopyData（'d'），将数据追加到 pg->pack 的 PGPACK_COPY_OUT 累积缓冲区
static void _pgpack_copy_data(pgpack_ctx *pgpack, binary_ctx *breader) {
    pgpack_copy_out_ctx *copyout = pgpack->pack;
    size_t datalen = breader->size - breader->offset;
    if (0 == datalen) {
        return;
    }
    binary_set_string(&copyout->data, breader->data + breader->offset, datalen);
}
// 解析一个完整的服务端消息，在收到 ReadyForQuery 时返回已累积的 pgpack_ctx
pgpack_ctx *_pgpack_parser(pgsql_ctx *pg, binary_ctx *breader, ud_cxt *ud, int32_t *status) {
    (void)ud;
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
        if (ERR_OK != _pgpack_row_description(pg->pack, breader)) {
            BIT_SET(*status, PROT_ERROR);
        }
        FREE(breader->data);
        break;
    case 'D': // DataRow：数据行，追加到 reader（成功时不释放 breader->data，所有权转移；失败时函数内已释放）
        _pgpack_init(pg, PGPACK_OK);
        if (ERR_OK != _pgpack_data_row(pg->pack, breader)) {
            BIT_SET(*status, PROT_ERROR);
        }
        break;
    case 'G': // CopyInResponse：服务端请求客户端发送 COPY FROM STDIN 数据，立即返回给调用方
        pack = _pgpack_copy_in_response(breader);
        FREE(breader->data);
        break;
    case 'H': // CopyOutResponse：服务端即将发送 COPY TO STDOUT 数据，初始化累积缓冲区
        _pgpack_init(pg, PGPACK_COPY_OUT);
        _pgpack_copy_out_response(pg->pack, breader);
        FREE(breader->data);
        break;
    case 'd': // CopyData：服务端发来的 COPY OUT 数据，追加到累积缓冲区
        //合法序列必为 'H'（CopyOutResponse 初始化 pg->pack=PGPACK_COPY_OUT）后才能收到 'd'。
        //若 pg->pack 为 NULL（无 'H' 前置）或类型不符（前一个查询的 PGPACK_OK 累积中），
        //强转 pgpack_copy_out_ctx* 后 binary_set_string 会写到错误偏移 → 内存损坏 / 空指针解引用。
        if (NULL == pg->pack || PGPACK_COPY_OUT != pg->pack->type) {
            BIT_SET(*status, PROT_ERROR);
            FREE(breader->data);
            break;
        }
        _pgpack_copy_data(pg->pack, breader);
        FREE(breader->data);
        break;
    case 'c': // CopyDone（服务端发出）：COPY OUT 数据传输完毕，等待后续 CommandComplete + ReadyForQuery
        FREE(breader->data);
        break;
    case 'C': { // CommandComplete：命令完成，记录命令标签
        // pg->pack 已存在时（如 COPY OUT 累积中）直接写入 complete，避免类型不符警告
        if (NULL == pg->pack) {
            _pgpack_init(pg, PGPACK_OK);
        }
        char *complete = binary_get_string(breader, 0);
        if (!EMPTYSTR(complete)) {
            safe_fill_str(pg->pack->complete, sizeof(pg->pack->complete), complete);
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
