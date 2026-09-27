#include "protocol/mysql/mysql_pack.h"
#include "protocol/mysql/mysql_utils.h"
#include "protocol/mysql/mysql_parse.h"

// 组包收尾：回填 3 字节长度头 → 失败即释放缓冲并把 *size 归零 → 成功交出缓冲长度。
// 调用方固定写 if (ERR_OK != _mysql_pack_finish(...)) { return NULL; }，别在各处再抄一遍 binary_free。
// 与 mysql.c 的 _mysql_send_pack 是同一件事的两种收尾：那边直接 ev_send，这边把缓冲交回调用方
static inline int32_t _mysql_pack_finish(binary_ctx *bwriter, size_t *size) {
    if (ERR_OK != _mysql_set_payload_lens(bwriter)) {
        binary_free(bwriter);
        *size = 0;
        return ERR_FAILED;
    }
    *size = bwriter->offset;
    return ERR_OK;
}
void *mysql_pack_quit(size_t *size) {
    binary_ctx bwriter;
    binary_init_write(&bwriter, 0, 0);
    binary_set_integer(&bwriter, 1, 3, 1);
    binary_set_int8(&bwriter, 0);
    binary_set_uint8(&bwriter, MYSQL_QUIT);
    *size = bwriter.offset;
    return bwriter.data;
}
void *mysql_pack_selectdb(mysql_ctx *mysql, const char *database, size_t *size) {
    size_t lens = strlen(database);
    if (ERR_OK != safe_fill_str(mysql->pending_db, sizeof(mysql->pending_db), database)) {
        LOG_ERROR("mysql database name exceeds %zu bytes: %zu.", sizeof(mysql->pending_db) - 1, lens);
        *size = 0;
        return NULL;
    }
    mysql->id = 0;
    binary_ctx bwriter;
    binary_init_write(&bwriter, 0, 0);
    binary_set_integer(&bwriter, lens + 1, 3, 1);
    binary_set_uint8(&bwriter, mysql->id);
    binary_set_uint8(&bwriter, MYSQL_INIT_DB);
    binary_set_binary(&bwriter, database, lens);
    *size = bwriter.offset;
    mysql->cur_cmd = MYSQL_INIT_DB;
    return bwriter.data;
}
void *mysql_pack_ping(mysql_ctx *mysql, size_t *size) {
    mysql->id = 0;
    binary_ctx bwriter;
    binary_init_write(&bwriter, 0, 0);
    binary_set_integer(&bwriter, 1, 3, 1);
    binary_set_uint8(&bwriter, mysql->id);
    binary_set_uint8(&bwriter, MYSQL_PING);
    *size = bwriter.offset;
    mysql->cur_cmd = MYSQL_PING;
    return bwriter.data;
}
void *mysql_pack_query(mysql_ctx *mysql, const char *sql, mysql_bind_ctx *mbind, size_t *size) {
    size_t sqllen = strlen(sql);
    if (sqllen >= INT3_MAX) {
        LOG_WARN("mysql payload exceeds 16MB: %zu bytes.", sqllen + 1);
        *size = 0;
        return NULL;
    }
    mysql->id = 0;
    size_t hint = 5 + 18 + sqllen;
    if (NULL != mbind) {
        hint += mbind->bitmap.offset + 1 + mbind->type_name.offset + mbind->value.offset;
    }
    binary_ctx bwriter;
    binary_init_write(&bwriter, hint, 0);
    binary_set_skip(&bwriter, 3);
    binary_set_uint8(&bwriter, mysql->id);
    binary_set_uint8(&bwriter, MYSQL_QUERY);//command
    if (BIT_CHECK(mysql->client.caps, CLIENT_QUERY_ATTRIBUTES)) {
        size_t count = 0;
        if (NULL != mbind
            && 0 != mbind->count) {
            count = (size_t)mbind->count;
        }
        _mysql_set_lenenc(&bwriter, count);//parameter_count
        _mysql_set_lenenc(&bwriter, 1);//parameter_set_count
        if (count > 0) {
            binary_set_binary(&bwriter, mbind->bitmap.data, mbind->bitmap.offset);//null_bitmap
            binary_set_int8(&bwriter, 1);//new_params_bind_flag,类型恒随包重发所以写死 1
            binary_set_binary(&bwriter, mbind->type_name.data, mbind->type_name.offset);//param_type_and_flag parameter name
            binary_set_binary(&bwriter, mbind->value.data, mbind->value.offset);//parameter_values
        }
    }
    binary_set_binary(&bwriter, sql, sqllen);//query
    if (ERR_OK != _mysql_pack_finish(&bwriter, size)) {
        return NULL;
    }
    mysql->cur_cmd = MYSQL_QUERY;
    mysql->parse_status = 0;
    return bwriter.data;
}
void *mysql_pack_stmt_prepare(mysql_ctx *mysql, const char *sql, size_t *size) {
    mysql->id = 0;
    size_t lens = strlen(sql);
    binary_ctx bwriter;
    binary_init_write(&bwriter, 5 + lens, 0);
    binary_set_skip(&bwriter, 3);
    binary_set_uint8(&bwriter, mysql->id);
    binary_set_uint8(&bwriter, MYSQL_PREPARE);
    binary_set_binary(&bwriter, sql, lens);
    if (ERR_OK != _mysql_pack_finish(&bwriter, size)) {
        return NULL;
    }
    mysql->cur_cmd = MYSQL_PREPARE;
    mysql->parse_status = 0;
    return bwriter.data;
}
void *mysql_pack_stmt_execute(mysql_stmt_ctx *stmt, mysql_bind_ctx *mbind, size_t *size) {
    size_t count = (NULL == mbind || mbind->count < 0) ? 0 : (size_t)mbind->count;
    if (count != (size_t)stmt->params_count) {
        *size = 0;
        return NULL;
    }
    stmt->mysql->id = 0;
    size_t hint = 14;
    if (count > 0) {
        hint += 9 + mbind->bitmap.offset + 1 + mbind->type_name.offset + mbind->type.offset + mbind->value.offset;
    }
    binary_ctx bwriter;
    binary_init_write(&bwriter, hint, 0);
    binary_set_skip(&bwriter, 3);
    binary_set_uint8(&bwriter, stmt->mysql->id);
    binary_set_uint8(&bwriter, MYSQL_EXECUTE);//status
    binary_set_integer(&bwriter, stmt->stmt_id, 4, 1);//statement_id
    binary_set_int8(&bwriter, 0);//flags
    binary_set_integer(&bwriter, 1, 4, 1);//iteration_count
    if (count > 0) {
        if (BIT_CHECK(stmt->mysql->client.caps, CLIENT_QUERY_ATTRIBUTES)) {
            _mysql_set_lenenc(&bwriter, count);//parameter_count
        }
        binary_set_binary(&bwriter, mbind->bitmap.data, mbind->bitmap.offset);//null_bitmap
        binary_set_int8(&bwriter, 1);//new_params_bind_flag,类型恒随包重发所以写死 1
        if (BIT_CHECK(stmt->mysql->client.caps, CLIENT_QUERY_ATTRIBUTES)) {
            binary_set_binary(&bwriter, mbind->type_name.data, mbind->type_name.offset);//parameter_type parameter_name
        } else {
            binary_set_binary(&bwriter, mbind->type.data, mbind->type.offset);//parameter_type
        }
        binary_set_binary(&bwriter, mbind->value.data, mbind->value.offset);//parameter_values
    }
    if (ERR_OK != _mysql_pack_finish(&bwriter, size)) {
        return NULL;
    }
    stmt->mysql->cur_cmd = MYSQL_EXECUTE;
    stmt->mysql->parse_status = 0;
    return bwriter.data;
}
void *mysql_pack_stmt_reset(mysql_stmt_ctx *stmt, size_t *size) {
    stmt->mysql->id = 0;
    binary_ctx bwriter;
    binary_init_write(&bwriter, 0, 0);
    binary_set_integer(&bwriter, 5, 3, 1);
    binary_set_uint8(&bwriter, stmt->mysql->id);
    binary_set_uint8(&bwriter, MYSQL_STMT_RESET);
    binary_set_integer(&bwriter, stmt->stmt_id, 4, 1);
    *size = bwriter.offset;
    stmt->mysql->cur_cmd = MYSQL_STMT_RESET;
    return bwriter.data;
}
void *mysql_pack_stmt_close(mysql_stmt_ctx *stmt, size_t *size) {
    stmt->mysql->id = 0;
    binary_ctx bwriter;
    binary_init_write(&bwriter, 0, 0);
    binary_set_integer(&bwriter, 5, 3, 1);
    binary_set_uint8(&bwriter, stmt->mysql->id);
    binary_set_uint8(&bwriter, MYSQL_STMT_CLOSE);
    binary_set_integer(&bwriter, stmt->stmt_id, 4, 1);
    *size = bwriter.offset;
    return bwriter.data;
}
void mysql_stmt_free(mysql_stmt_ctx *stmt) {
    _mpack_stm_free(stmt);
    FREE(stmt);
}
