#include "protocol/pgsql/pgsql_parse.h"
#include "utils/utils.h"

// 解析 ErrorResponse / NoticeResponse，将各字段拼接为可读字符串返回（调用方负责释放）
char *_pgpack_error_notice(binary_ctx *breader) {
    char flag;
    char *tmp;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    for (;;) {
        if (!binary_have(breader, 1)) {
            break;
        }
        flag = binary_get_int8(breader); // 字段类型标志（如 'S'=严重性, 'M'=消息等）
        if (0 == flag) {
            break;
        }
        tmp = binary_try_get_string(breader);
        if (NULL == tmp) {
            break;// 字段值没收全，前面拼出来的照常交出去
        }
        binary_set_int8(&bwriter, flag);
        binary_set_binary(&bwriter, ": ", 2);
        if (binary_remain(breader) > 1) { // 还有后续字段（1 字节为结束标志）
            binary_set_va(&bwriter, "%s\r\n", tmp);
        } else {
            binary_set_string(&bwriter, tmp); // 最后一个字段，不追加换行
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
void _pgpack_results_clear(pgpack_ctx *pgpack) {
    pgsql_result *res;
    for (uint32_t i = 0; i < array_size(&pgpack->results); i++) {
        res = array_at(&pgpack->results, i);
        if (NULL != res->reader) {
            _pgpack_reader_free(res->reader);
            FREE(res->reader);
        }
    }
    array_free(&pgpack->results);
    // array_free 只复位 size/maxsize，elsize 原样留着，而下面拿 elsize 当"数组还没建"的判据
    pgpack->results.elsize = 0;
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
        // 已提交的结果一并丢弃：COPY 那条语句不占位，留着前面的结果只会让下标与语句序号错开，
        // 调用方按下标取就会拿到别的语句的数据。宁可一条都不给
        _pgpack_results_clear(pg->pack);
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
    _pgpack_results_clear(pgpack);
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
// 解析 NotificationResponse（'A'），返回新分配的 pgpack_ctx（PGPACK_NOTIFICATION 类型）；
// 报文残缺返回 NULL，此时未接管 breader->data，由调用方释放
static pgpack_ctx *_pgpack_notification_response(binary_ctx *breader) {
    // 三个字段都读出来再分配：中途失败就不必回滚已经转移出去的所有权
    if (!binary_have(breader, 4)) {
        return NULL;
    }
    int32_t pid = (int32_t)binary_get_integer(breader, 4, 0);
    char *channel = binary_try_get_string(breader);
    char *content = (NULL != channel) ? binary_try_get_string(breader) : NULL;
    if (NULL == content) {
        return NULL;
    }
    pgpack_notification *notification;
    MALLOC(notification, sizeof(pgpack_notification));
    notification->payload = breader->data; // 接管原始消息缓冲区的所有权
    notification->pid = pid;
    notification->channel = channel;
    notification->notification = content;
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
    // 上一条语句的 reader 在 CommandComplete 时已提交进结果数组，这里 fields 仍非空
    // 只能是两个 RowDescription 之间没有 CommandComplete 的违规消息流
    if (NULL != reader->fields) {
        LOG_WARN("protocol violation: second RowDescription without CommandComplete in between.");
        return ERR_FAILED;
    }
    if (!binary_have(breader, 2)) {
        return ERR_FAILED;
    }
    reader->field_count = (uint16_t)binary_get_uinteger(breader, 2, 0);
    if (0 == reader->field_count) {
        return ERR_OK;
    }
    size_t remaining = binary_remain(breader);
    if ((size_t)reader->field_count * 19 > remaining) {
        reader->field_count = 0;
        return ERR_FAILED;
    }
    MALLOC(reader->fields, sizeof(pgpack_field) * (size_t)reader->field_count);
    char *fname;
    size_t nlens;
    pgpack_field *field;
    for (uint16_t i = 0; i < reader->field_count; i++) {
        field = &reader->fields[i];
        // 上面的 19 字节/列只保证了总量，单个列名超长仍会把后面的列挤出报文，逐列再判一次
        fname = binary_try_get_string(breader);
        if (NULL == fname
            || !binary_have(breader, 18)) {// table_oid(4) index(2) type_oid(4) lens(2) modifier(4) format(2)
            reader->field_count = 0;
            FREE(reader->fields);
            return ERR_FAILED;
        }
        nlens = strlen(fname);
        if (ERR_OK != safe_fill_str(field->name, sizeof(field->name), fname)) {
            // fields 是 MALLOC 出来的，而 safe_fill_str 装不下时一个字节都不写：留着就是
            // 未初始化内存被 _pgsql_reader_index 的 strcmp 读，还未必有 NUL。置空串，
            // 效果是这一列按名查不到（按下标仍可取）
            field->name[0] = '\0';
            LOG_ERROR("pgsql field name exceeds %zu bytes: %zu, column dropped from name lookup; "
                      "stock servers truncate at NAMEDATALEN-1 = 63, this one was built with a larger one.",
                      sizeof(field->name) - 1, nlens);
        }
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
    if (!binary_have(breader, 2)) {
        FREE(breader->data);
        return ERR_FAILED;
    }
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
        if (!binary_have(breader, 4)) {// 列长度字段本身也可能被截断
            FREE(rows);
            FREE(breader->data);
            return ERR_FAILED;
        }
        row->lens = (int32_t)binary_get_integer(breader, 4, 0);
        if (row->lens > 0) {
            if (!binary_have(breader, (size_t)row->lens)) {
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
// 解析 CopyInResponse（'G'），返回新分配的 pgpack_ctx（PGPACK_COPY_IN 类型，立即返回给调用方）；
// 报文残缺返回 NULL
static pgpack_ctx *_pgpack_copy_in_response(binary_ctx *breader) {
    if (!binary_have(breader, 3)) {// format(1) + ncol(2)
        return NULL;
    }
    pgpack_copy_in_ctx *copyin;
    MALLOC(copyin, sizeof(pgpack_copy_in_ctx));
    copyin->format = (pgpack_format)binary_get_int8(breader);
    copyin->ncol = (int16_t)binary_get_integer(breader, 2, 0);
    pgpack_ctx *pgpack = _pgpack_init(NULL, PGPACK_COPY_IN);
    pgpack->pack = copyin;
    return pgpack;
}
// 解析 CopyOutResponse（'H'），初始化 pg->pack 中的 PGPACK_COPY_OUT 累积缓冲区；
// 报文残缺返回 ERR_FAILED，此时不动 pgpack
static int32_t _pgpack_copy_out_response(pgpack_ctx *pgpack, binary_ctx *breader) {
    if (!binary_have(breader, 3)) {// format(1) + ncol(2)
        return ERR_FAILED;
    }
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
    return ERR_OK;
}
// 解析 CopyData（'d'），将数据追加到 pg->pack 的 PGPACK_COPY_OUT 累积缓冲区
static void _pgpack_copy_data(pgpack_ctx *pgpack, binary_ctx *breader) {
    pgpack_copy_out_ctx *copyout = pgpack->pack;
    size_t datalen = binary_remain(breader);
    if (0 == datalen) {
        return;
    }
    binary_set_binary(&copyout->data, breader->data + breader->offset, datalen);
}
// 解析 CommandComplete（'C'），记录命令标签，并按语句边界把当前累积的结果提交进结果数组
static int32_t _pgpack_complete(pgsql_ctx *pg, binary_ctx *breader) {
    // pg->pack 已存在时（如 COPY OUT 累积中）直接写入 complete，避免类型不符警告
    if (NULL == pg->pack) {
        _pgpack_init(pg, PGPACK_OK);
    }
    char *complete = binary_try_get_string(breader);
    if (NULL == complete) {
        return ERR_FAILED;
    }
    // 先清再填：多语句时残留上一条的标签会被错当本条的 affected_rows
    pg->pack->complete[0] = '\0';
    if (!EMPTYSTR(complete)
        && ERR_OK != safe_fill_str(pg->pack->complete, sizeof(pg->pack->complete), complete)) {
        LOG_ERROR("pgsql command tag exceeds %zu bytes: %zu, affected_rows unavailable.",
                  sizeof(pg->pack->complete) - 1, strlen(complete));
    }
    // 只有普通查询按语句提交结果（reader 或无结果集的 NULL）；COPY 的数据仍由 pack 持有
    if (PGPACK_OK != pg->pack->type) {
        return ERR_OK;
    }
    if (0 == pg->pack->results.elsize) {// 首次提交才建数组，无结果可提交的包（通知 / 认证期 / COPY OUT）不分配
        array_init(&pg->pack->results, sizeof(pgsql_result), 2);
    }
    pgsql_result res;
    ZERO(&res, sizeof(res));// 逐字段写满是当前字段表的巧合，加字段就会把栈上残留拷进数组
    res.reader = pg->pack->pack;
    memcpy(res.complete, pg->pack->complete, sizeof(res.complete));
    array_push_back(&pg->pack->results, &res);
    pg->pack->pack = NULL; // reader 所有权移入结果数组
    pg->pack->_free_pgpack = NULL;
    return ERR_OK;
}
// 解析一个完整的服务端消息，在收到 ReadyForQuery 时返回已累积的 pgpack_ctx
pgpack_ctx *_pgpack_parser(pgsql_ctx *pg, binary_ctx *breader, ud_cxt *ud, int32_t *status) {
    (void)ud;
    pgpack_ctx *pack = NULL;
    int8_t code = binary_get_int8(breader); // 读取消息类型码
    binary_get_skip(breader, 4); // 跳过消息体长度字段
    switch (code) { // N / S / A 随时都有可能收到（异步消息）
    case 'N': // NoticeResponse：服务端通知消息，忽略
        FREE(breader->data);
        break;
    case 'S': // ParameterStatus：运行时参数状态报告，忽略
        FREE(breader->data);
        break;
    case 'A': // NotificationResponse：LISTEN 产生的异步通知，立即返回给上层
        pack = _pgpack_notification_response(breader);
        if (NULL == pack) {
            BIT_SET(*status, PROT_ERROR);
            FREE(breader->data);
        }
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
    case '1': // ParseComplete：Parse 命令完成
    case '2': // BindComplete：Bind 命令完成
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
        if (NULL == pack) {
            BIT_SET(*status, PROT_ERROR);
        }
        FREE(breader->data);
        break;
    case 'H': // CopyOutResponse：服务端即将发送 COPY TO STDOUT 数据，初始化累积缓冲区
        _pgpack_init(pg, PGPACK_COPY_OUT);
        if (ERR_OK != _pgpack_copy_out_response(pg->pack, breader)) {
            BIT_SET(*status, PROT_ERROR);
        }
        FREE(breader->data);
        break;
    case 'd': // CopyData：服务端发来的 COPY OUT 数据，追加到累积缓冲区
        //合法序列必为 'H'（CopyOutResponse 初始化 pg->pack=PGPACK_COPY_OUT）后才能收到 'd'。
        //若 pg->pack 为 NULL（无 'H' 前置）或类型不符（前一个查询的 PGPACK_OK 累积中），
        //强转 pgpack_copy_out_ctx* 后 binary_set_binary 会写到错误偏移 → 内存损坏 / 空指针解引用。
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
    case 'C': // CommandComplete：命令完成，记录命令标签并按语句边界提交一个结果
        if (ERR_OK != _pgpack_complete(pg, breader)) {
            BIT_SET(*status, PROT_ERROR);
        }
        FREE(breader->data);
        break;
    case 'Z': // ReadyForQuery：服务端就绪，将累积的结果包返回给调用方
        if (!binary_have(breader, 1)) {
            BIT_SET(*status, PROT_ERROR);
            FREE(breader->data);
            break;
        }
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
