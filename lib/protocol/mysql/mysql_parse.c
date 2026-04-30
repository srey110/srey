#include "protocol/mysql/mysql_parse.h"
#include "protocol/mysql/mysql_utils.h"
#include "protocol/prots.h"
#include "utils/utils.h"

// 结果集解析状态：字段描述阶段 / 行数据阶段
typedef enum RST_STATUS {
    RST_FIELD = 0x01,  // 正在解析列字段描述
    RST_ROW            // 正在解析行数据
}RST_STATUS;

// 预处理语句准备响应解析状态：参数字段阶段 / 结果集字段阶段
typedef enum STMT_PREPARE_STATUS {
    STMT_PREPARE_PARAMS = 0x01, // 正在解析参数字段描述
    STMT_PREPARE_FIELD          // 正在解析结果集字段描述
}STMT_PREPARE_STATUS;

// 解析 MySQL 数据包头部（4 字节），输出 payload 长度；数据不足时返回 ERR_FAILED
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
    int32_t _rtn;
    uint64_t size = _mysql_get_lenenc(breader, &_rtn);
    ok->affected_rows = (int64_t)size;
    size = _mysql_get_lenenc(breader, &_rtn);
    ok->last_insert_id = (int64_t)size;
    ok->status_flags = (int16_t)binary_get_integer(breader, 2, 1);
    ok->warnings = (int16_t)binary_get_integer(breader, 2, 1);
    binary_get_skip(breader, breader->size - breader->offset);
    mysql->last_id = ok->last_insert_id;
    mysql->affected_rows = ok->affected_rows;
}
// 解析 EOF 响应包，读取警告数和状态标志
static void _mpack_eof(binary_ctx *breader, mpack_eof *eof) {
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
// 分配并初始化一个新的 mpack_ctx，关联当前序列号和 payload 指针
static mpack_ctx *_mpack_new(mysql_ctx *mysql, char *payload) {
    mpack_ctx *mpack;
    CALLOC(mpack, 1, sizeof(mpack_ctx));
    mpack->sequence_id = mysql->id;
    mpack->payload = payload;
    return mpack;
}
// 解析 COM_INIT_DB 响应包（OK 或 ERR）
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
// 解析 COM_PING 响应包（固定为 OK）
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
// 初始化结果集读取器并挂载到 mysql->mpack，准备接收列字段描述
static void _mpack_reader_new(mysql_ctx *mysql, binary_ctx *breader, mpack_type pktype) {
    int32_t _rtn;
    mysql->mpack = _mpack_new(mysql, NULL);
    mysql_reader_ctx *reader;
    CALLOC(reader, 1, sizeof(mysql_reader_ctx));
    reader->pack_type = pktype;
    reader->field_count = (int32_t)_mysql_get_lenenc(breader, &_rtn);//column_count
    if (reader->field_count > 0) {
        CALLOC(reader->fields, 1, sizeof(mpack_field) * (size_t)reader->field_count);
    }
    arr_ptr_init(&reader->arr_rows, 64);
    mysql->mpack->pack = reader;
    mysql->mpack->_free_mpack = _mpack_reader_free;
    mysql->mpack->pack_type = pktype;
}
// 从缓冲区继续读取下一个 MySQL 数据包，并初始化 breader 指向新 payload
static int32_t _mpack_more_data(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status) {
    size_t payload_lens;
    char *payload = _mysql_payload(mysql, buf, &payload_lens, status);
    if (NULL == payload) {
        return ERR_FAILED;
    }
    binary_init(breader, payload, payload_lens, 0);
    return ERR_OK;
}
// 检查 EOF 包中的状态标志：若有更多结果集则设置 PROT_MOREDATA，否则返回 ERR_OK 表示结束
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
// 解析文本协议（COM_QUERY）结果集中的一行数据，字段值以 lenenc 字符串存储
static void _mpack_parse_text_row(mysql_reader_ctx *reader, binary_ctx *breader) {
    int32_t _rtn;
    mpack_row *row;
    CALLOC(row, 1, sizeof(mpack_row) * (size_t)reader->field_count);
    row->payload = breader->data;
    for (int32_t i = 0; i < reader->field_count; i++) {
        if (0xfb == (uint8_t)(binary_at(breader, breader->offset)[0])) {
            row[i].nil = 1; // 0xfb 表示 NULL 值
            binary_get_skip(breader, 1);
            continue;
        }
        row[i].val.lens = (size_t)_mysql_get_lenenc(breader, &_rtn);
        if (row[i].val.lens > 0) {
            row[i].val.data = binary_get_string(breader, row[i].val.lens);
        }
    }
    arr_ptr_push_back(&reader->arr_rows, (void **)&row);
}
// 解析二进制协议（COM_STMT_EXECUTE）结果集中的一行数据，字段值按类型固定或 lenenc 长度读取
static int32_t _mpack_parse_binary_row(mysql_reader_ctx *reader, binary_ctx *breader) {
    int32_t off;
    int32_t _rtn;
    mpack_row *row;
    CALLOC(row, 1, sizeof(mpack_row) * (size_t)reader->field_count);
    row->payload = breader->data;
    // 读取 NULL 位图（偏移量 +2 是因为二进制协议位图从第 3 位开始）
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
            // 日期/时间类型以 1 字节长度前缀编码
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
            // 字符串/BLOB 类型以 lenenc 长度编码
            row[i].val.lens = (size_t)_mysql_get_lenenc(breader, &_rtn);
            if (ERR_OK != _rtn) {
                FREE(row);
                return ERR_FAILED;
            }
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
// 循环读取并解析结果集行数据，直到遇到 EOF 包结束
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
            mpack_ctx *mpack = mysql->mpack; // 行解析完成，返回完整结果集
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
// 解析单个列字段描述包（Column Definition），填充 mpack_field 结构体
static void _mpack_parse_field(binary_ctx *breader, mpack_field *field) {
    int32_t _rtn;
    char *val;
    size_t cplen;
    uint64_t lens = _mysql_get_lenenc(breader, &_rtn);
    binary_get_skip(breader, (size_t)lens);//catalog（跳过 catalog 字段）
    lens = _mysql_get_lenenc(breader, &_rtn);
    if (lens > 0) {
        val = binary_get_string(breader, (size_t)lens);
        cplen = lens < sizeof(field->schema) ? (size_t)lens : sizeof(field->schema) - 1;
        memcpy(field->schema, val, cplen);
        field->schema[cplen] = '\0';
    }
    lens = _mysql_get_lenenc(breader, &_rtn);
    if (lens > 0) {
        val = binary_get_string(breader, (size_t)lens);
        cplen = lens < sizeof(field->table) ? (size_t)lens : sizeof(field->table) - 1;
        memcpy(field->table, val, cplen);
        field->table[cplen] = '\0';
    }
    lens = _mysql_get_lenenc(breader, &_rtn);
    if (lens > 0) {
        val = binary_get_string(breader, (size_t)lens);
        cplen = lens < sizeof(field->org_table) ? (size_t)lens : sizeof(field->org_table) - 1;
        memcpy(field->org_table, val, cplen);
        field->org_table[cplen] = '\0';
    }
    lens = _mysql_get_lenenc(breader, &_rtn);
    if (lens > 0) {
        val = binary_get_string(breader, (size_t)lens);
        cplen = lens < sizeof(field->name) ? (size_t)lens : sizeof(field->name) - 1;
        memcpy(field->name, val, cplen);
        field->name[cplen] = '\0';
    }
    lens = _mysql_get_lenenc(breader, &_rtn);
    if (lens > 0) {
        val = binary_get_string(breader, (size_t)lens);
        cplen = lens < sizeof(field->org_name) ? (size_t)lens : sizeof(field->org_name) - 1;
        memcpy(field->org_name, val, cplen);
        field->org_name[cplen] = '\0';
    }
    _mysql_get_lenenc(breader, &_rtn);//length of fixed length fields（跳过固定长度字段的长度标志）
    field->character = (int16_t)binary_get_integer(breader, 2, 1);
    field->field_lens = (int32_t)binary_get_integer(breader, 4, 1);
    field->type = binary_get_uint8(breader);
    field->flags = (uint16_t)binary_get_uinteger(breader, 2, 1);
    field->decimals = binary_get_uint8(breader);
}
// 循环读取并解析结果集列字段描述，字段解析完毕后继续解析行数据
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
            mysql->parse_status = RST_ROW; // 字段解析完成，切换到行解析阶段
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
// 根据当前解析状态（字段阶段/行阶段）分发到对应处理函数
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
// 解析 COM_QUERY 响应：可能是 OK/ERR 或带字段+行数据的完整结果集
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
            // 不支持 LOCAL INFILE，直接报错
            BIT_SET(*status, PROT_ERROR);
            FREE(breader->data);
            break;
        default:
            // 结果集响应：先读取列数
            _mpack_reader_new(mysql, breader, MPACK_QUERY);
            FREE(breader->data);
            mysql->parse_status = RST_FIELD;
            if (ERR_OK != _mpack_more_data(mysql, buf, breader, status)) {
                break;
            }
            mpack = _mpack_reader(mysql, buf, breader, status);
            break;
        }
    } else {
        // 续接解析（数据分片场景）
        mpack = _mpack_reader(mysql, buf, breader, status);
    }
    return mpack;
}
// 循环读取并解析 STMT_PREPARE 响应中的参数字段和结果集字段描述
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
                    // 参数字段解析完成，切换到结果集字段解析阶段
                    mysql->parse_status = STMT_PREPARE_FIELD;
                    stmt->index = 0;
                    if (ERR_OK != _mpack_more_data(mysql, buf, breader, status)) {
                        return NULL;
                    }
                    continue;
                } else {
                    // 无结果集字段，直接返回
                    mpack_ctx *mpack = mysql->mpack;
                    mysql->mpack = NULL;
                    mysql->cur_cmd = 0;
                    return mpack;
                }
            } else {
                // 结果集字段解析完成，返回
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
    // 将所有权从 mpack 转移给调用方，避免重复释放
    mpack->pack = NULL;
    mpack->_free_mpack = NULL;
    return stmt;
}
void _mpack_stm_free(void *pack) {
    mysql_stmt_ctx *stmt = pack;
    FREE(stmt->params);
    FREE(stmt->fields);
}
// 分配并初始化预处理语句上下文，从 STMT_PREPARE OK 响应包中读取 stmt_id、字段数和参数数
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
        // 参数字段优先解析（覆盖字段阶段状态）
        mysql->parse_status = STMT_PREPARE_PARAMS;
        CALLOC(stmt->params, 1, sizeof(mpack_field) * (size_t)stmt->params_count);
    }
    mysql->mpack->pack = stmt;
    mysql->mpack->_free_mpack = _mpack_stm_free;
}
// 解析 COM_STMT_PREPARE 响应：ERR 直接返回，OK 后继续解析参数和字段描述
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
            // 无参数无字段，立即返回
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
        // 续接解析（数据分片场景）
        return _mpack_stmt(mysql, buf, breader, status);
    }
}
// 解析 COM_STMT_EXECUTE 响应：可能是 OK/ERR 或带字段+行数据的二进制结果集
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
            // 二进制结果集响应：先读取列数
            _mpack_reader_new(mysql, breader, MPACK_STMT_EXECUTE);
            FREE(breader->data);
            mysql->parse_status = RST_FIELD;
            if (ERR_OK != _mpack_more_data(mysql, buf, breader, status)) {
                break;
            }
            mpack = _mpack_reader(mysql, buf, breader, status);
            break;
        }
    } else {
        // 续接解析（数据分片场景）
        mpack = _mpack_reader(mysql, buf, breader, status);
    }
    return mpack;
}
// 解析 COM_STMT_RESET 响应（OK 或 ERR）
static mpack_ctx *_reset_response(mysql_ctx *mysql, binary_ctx *breader) {
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
        mpack = _reset_response(mysql, breader);
        break;
    default:
        FREE(breader->data);
        BIT_SET(*status, PROT_ERROR);
        break;
    }
    return mpack;
}
