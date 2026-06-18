#include "protocol/mysql/mysql_pack.h"
#include "protocol/mysql/mysql_utils.h"
#include "protocol/mysql/mysql_parse.h"

void *mysql_pack_quit(mysql_ctx *mysql, size_t *size) {
    mysql->id = 0;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_integer(&bwriter, 1, 3, 1);
    binary_set_int8(&bwriter, mysql->id);
    binary_set_uint8(&bwriter, MYSQL_QUIT);
    *size = bwriter.offset;
    mysql->cur_cmd = MYSQL_QUIT;
    return bwriter.data;
}
void *mysql_pack_selectdb(mysql_ctx *mysql, const char *database, size_t *size) {
    mysql->id = 0;
    size_t lens = strlen(database);
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_integer(&bwriter, lens + 1, 3, 1);
    binary_set_int8(&bwriter, mysql->id);
    binary_set_uint8(&bwriter, MYSQL_INIT_DB);
    binary_set_binary(&bwriter, database, lens);
    *size = bwriter.offset;
    mysql->cur_cmd = MYSQL_INIT_DB;
    return bwriter.data;
}
void *mysql_pack_ping(mysql_ctx *mysql, size_t *size) {
    mysql->id = 0;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_integer(&bwriter, 1, 3, 1);
    binary_set_int8(&bwriter, mysql->id);
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
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_skip(&bwriter, 3);
    binary_set_int8(&bwriter, mysql->id);
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
            int8_t bind_flag = 1;
            binary_set_int8(&bwriter, bind_flag);//new_params_bind_flag
            if (bind_flag) {
                binary_set_binary(&bwriter, mbind->type_name.data, mbind->type_name.offset);//param_type_and_flag parameter name
            }
            binary_set_binary(&bwriter, mbind->value.data, mbind->value.offset);//parameter_values
        }
    }
    binary_set_binary(&bwriter, sql, sqllen);//query
    if (ERR_OK != _mysql_set_payload_lens(&bwriter)) {
        binary_free(&bwriter);
        *size = 0;
        return NULL;
    }
    *size = bwriter.offset;
    mysql->cur_cmd = MYSQL_QUERY;
    mysql->parse_status = 0;
    return bwriter.data;
}
void *mysql_pack_stmt_prepare(mysql_ctx *mysql, const char *sql, size_t *size) {
    mysql->id = 0;
    size_t lens = strlen(sql);
    if (lens + 1 > INT3_MAX) {
        LOG_WARN("mysql payload exceeds 16MB: %zu bytes.", lens + 1);
        *size = 0;
        return NULL;
    }
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_integer(&bwriter, lens + 1, 3, 1);
    binary_set_int8(&bwriter, mysql->id);
    binary_set_uint8(&bwriter, MYSQL_PREPARE);
    binary_set_binary(&bwriter, sql, lens);
    *size = bwriter.offset;
    mysql->cur_cmd = MYSQL_PREPARE;
    mysql->parse_status = 0;
    return bwriter.data;
}
void *mysql_pack_stmt_execute(mysql_stmt_ctx *stmt, mysql_bind_ctx *mbind, size_t *size) {
    if (stmt->params_count > 0
        && (NULL == mbind || (size_t)mbind->count != (size_t)stmt->params_count)) {
        *size = 0;
        return NULL;
    }
    stmt->mysql->id = 0;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_skip(&bwriter, 3);
    binary_set_int8(&bwriter, stmt->mysql->id);
    binary_set_uint8(&bwriter, MYSQL_EXECUTE);//status
    binary_set_integer(&bwriter, stmt->stmt_id, 4, 1);//statement_id
    binary_set_int8(&bwriter, 0);//flags
    binary_set_integer(&bwriter, 1, 4, 1);//iteration_count
    if (stmt->params_count > 0) {
        size_t count = 0;
        if (NULL != mbind
            && 0 != mbind->count) {
            count = (size_t)mbind->count;
        }
        if (BIT_CHECK(stmt->mysql->client.caps, CLIENT_QUERY_ATTRIBUTES)) {
            _mysql_set_lenenc(&bwriter, count);//parameter_count
        }
        if (count > 0) {
            binary_set_binary(&bwriter, mbind->bitmap.data, mbind->bitmap.offset);//null_bitmap
            int8_t bind_flag = 1;
            binary_set_int8(&bwriter, bind_flag);//new_params_bind_flag
            if (bind_flag) {
                if (BIT_CHECK(stmt->mysql->client.caps, CLIENT_QUERY_ATTRIBUTES)) {
                    binary_set_binary(&bwriter, mbind->type_name.data, mbind->type_name.offset); //parameter_type parameter_name
                } else {
                    binary_set_binary(&bwriter, mbind->type.data, mbind->type.offset);//parameter_type
                }
            }
            binary_set_binary(&bwriter, mbind->value.data, mbind->value.offset);//parameter_values
        }
    }
    if (ERR_OK != _mysql_set_payload_lens(&bwriter)) {
        binary_free(&bwriter);
        *size = 0;
        return NULL;
    }
    *size = bwriter.offset;
    stmt->mysql->cur_cmd = MYSQL_EXECUTE;
    stmt->mysql->parse_status = 0;
    return bwriter.data;
}
void *mysql_pack_stmt_reset(mysql_stmt_ctx *stmt, size_t *size) {
    stmt->mysql->id = 0;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_integer(&bwriter, 5, 3, 1);
    binary_set_int8(&bwriter, stmt->mysql->id);
    binary_set_uint8(&bwriter, MYSQL_STMT_RESET);
    binary_set_integer(&bwriter, stmt->stmt_id, 4, 1);
    *size = bwriter.offset;
    stmt->mysql->cur_cmd = MYSQL_STMT_RESET;
    return bwriter.data;
}
void *mysql_pack_stmt_close(mysql_stmt_ctx *stmt, size_t *size) {
    stmt->mysql->id = 0;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_integer(&bwriter, 5, 3, 1);
    binary_set_int8(&bwriter, stmt->mysql->id);
    binary_set_uint8(&bwriter, MYSQL_STMT_CLOSE);
    binary_set_integer(&bwriter, stmt->stmt_id, 4, 1);
    *size = bwriter.offset;
    // 释放语句内部资源并销毁 stmt 对象
    _mpack_stm_free(stmt);
    FREE(stmt);
    return bwriter.data;
}
