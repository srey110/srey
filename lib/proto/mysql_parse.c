#include "proto/mysql_parse.h"
#include "proto/mysql_macro.h"
#include "utils.h"

typedef enum resultset_metadata {
    RESULTSET_METADATA_NONE = 0,
    RESULTSET_METADATA_FULL = 1
}resultset_metadata;

static int32_t _mysql_get_lenenc_int(binary_ctx *breader, uint64_t *integer) {
    uint8_t flag = binary_get_uint8(breader);
    if (flag <= 0xfa) {
        *integer = flag;
        return ERR_OK;
    }
    if (0xfb == flag) {
        return 1;
    }
    if (0xfc == flag) {
        *integer = (uint64_t)binary_get_uint16(breader, 2, 1);
        return ERR_OK;
    }
    if (0xfd == flag) {
        *integer = (uint64_t)binary_get_uint32(breader, 3, 1);
        return ERR_OK;
    }
    if (0xfe == flag) {
        *integer = binary_get_uint64(breader, 8, 1);
        return ERR_OK;
    }
    LOG_ERROR("unknow int<lenenc>, %d.", (int32_t)flag);
    return ERR_FAILED;
}
void _mysql_set_lenenc_int(binary_ctx *bwriter, size_t integer) {
    if (integer <= 0xfa) {
        binary_set_uint8(bwriter, (uint8_t)integer);
        return;
    }
    if (integer <= USHRT_MAX) {
        binary_set_uint8(bwriter, 0xfc);
        binary_set_integer(bwriter, (int64_t)integer, 2, 1);
        return;
    }
    if (integer <= INT3_MAX) {
        binary_set_uint8(bwriter, 0xfd);
        binary_set_integer(bwriter, (int64_t)integer, 3, 1);
        return;
    }
    binary_set_uint8(bwriter, 0xfe);
    binary_set_integer(bwriter, (int64_t)integer, 8, 1);
}
int32_t _mpack_ok(binary_ctx *breader, mpack_ok *ok) {
    uint64_t size;
    _mysql_get_lenenc_int(breader, &size);
    ok->affected_rows = (int64_t)size;
    _mysql_get_lenenc_int(breader, &size);
    ok->last_insert_id = (int64_t)size;
    ok->status_flags = binary_get_int16(breader, 2, 1);
    ok->warnings = binary_get_int16(breader, 2, 1);
    return ERR_OK;
}
int32_t _mpack_eof(binary_ctx *breader, mpack_eof *eof) {
    eof->warnings = binary_get_int16(breader, 2, 1);
    eof->status_flags = binary_get_int16(breader, 2, 1);
    return ERR_OK;
}
int32_t _mpack_err(binary_ctx *breader, mpack_err *err) {
    err->error_code = binary_get_int16(breader, 2, 1);
    binary_get_skip(breader, 6);//sql_state_marker sql_state
    err->error_msg.lens = breader->size - breader->offset;
    err->error_msg.data = binary_get_string(breader, err->error_msg.lens);
    return ERR_OK;
}
static int32_t _quit_response(mysql_ctx *mysql, binary_ctx *breader, mpack_ctx *mpack) {
    mysql->cur_cmd = 0;
    MALLOC(mpack->mpack, sizeof(mpack_err));
    return _mpack_err(breader, mpack->mpack);
}
static int32_t _selectdb_response(mysql_ctx *mysql, binary_ctx *breader, mpack_ctx *mpack) {
    mysql->cur_cmd = 0;
    if (MYSQL_OK == mpack->command) {
        MALLOC(mpack->mpack, sizeof(mpack_ok));
        return _mpack_ok(breader, mpack->mpack);
    } else {
        MALLOC(mpack->mpack, sizeof(mpack_err));
        return _mpack_err(breader, mpack->mpack);
    }
}
static int32_t _ping_response(mysql_ctx *mysql, binary_ctx *breader, mpack_ctx *mpack) {
    mysql->cur_cmd = 0;
    MALLOC(mpack->mpack, sizeof(mpack_ok));
    return _mpack_ok(breader, mpack->mpack);
}
int32_t _mpack_parser(mysql_ctx *mysql, binary_ctx *breader, mpack_ctx *mpack) {
    int32_t rtn = ERR_FAILED;
    switch (mysql->cur_cmd) {
    case MYSQL_QUIT:
        rtn = _quit_response(mysql, breader, mpack);
        break;
    case MYSQL_INIT_DB:
        rtn = _selectdb_response(mysql, breader, mpack);
        break;
    case MYSQL_PING:
        rtn = _ping_response(mysql, breader, mpack);
        break;
    case MYSQL_QUERY:
        break;
    default:
        break;
    }
    return rtn;
}
