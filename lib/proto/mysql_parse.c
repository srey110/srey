#include "proto/mysql_parse.h"
#include "proto/mysql_macro.h"
#include "utils.h"

#define INT3_MAX 0xFFFFFF

static void _mysql_get_fixed_lens_integer(binary_ctx *breader, uint64_t *size) {
    uint8_t flag = binary_get_uint8(breader);
    if (flag >= 0x00 && flag <= 0xfa) {
        *size = flag;
    } else if (0xfc == flag) {
        *size = (uint64_t)binary_get_uint16(breader, 2, 1);
    } else if (0xfd == flag) {
        *size = (uint64_t)binary_get_uint32(breader, 3, 1);
    } else if (0xfe == flag) {
        *size = binary_get_uint64(breader, 8, 1);
    } else {
        *size = 0;
    }
}
void _mysql_set_fixed_lens_integer(binary_ctx *bwriter, size_t lens) {
    if (lens >= 0x00 && lens <= 0xfa) {
        binary_set_uint8(bwriter, (uint8_t)lens);
    } else if (lens > 0xfa && lens <= USHRT_MAX) {
        binary_set_uint8(bwriter, 0xfc);
        binary_set_integer(bwriter, (int64_t)lens, 2, 1);
    } else if (lens > USHRT_MAX && lens <= INT3_MAX) {
        binary_set_uint8(bwriter, 0xfd);
        binary_set_integer(bwriter, (int64_t)lens, 3, 1);
    } else if (lens > INT3_MAX && lens <= LLONG_MAX) {
        binary_set_uint8(bwriter, 0xfe);
        binary_set_integer(bwriter, (int64_t)lens, 8, 1);
    }
}
int32_t _mpack_ok(mysql_params *params, binary_ctx *breader, mpack_ok *ok) {
    uint64_t size;
    _mysql_get_fixed_lens_integer(breader, &size);
    ok->affected_rows = (int64_t)size;
    _mysql_get_fixed_lens_integer(breader, &size);
    ok->last_insert_id = (int64_t)size;
    ok->status_flags = binary_get_int16(breader, 2, 1);
    ok->warnings = binary_get_int16(breader, 2, 1);
    return ERR_OK;
}
int32_t _mpack_eof(mysql_params *params, binary_ctx *breader, mpack_eof *eof) {
    eof->warnings = binary_get_int16(breader, 2, 1);
    eof->status_flags = binary_get_int16(breader, 2, 1);
    return ERR_OK;
}
int32_t _mpack_err(mysql_params *params, binary_ctx *breader, mpack_err *err) {
    err->error_code = binary_get_int16(breader, 2, 1);
    binary_get_skip(breader, 6);//sql_state_marker sql_state
    err->error_msg.lens = breader->size - breader->offset;
    err->error_msg.data = binary_get_string(breader, err->error_msg.lens);
    return ERR_OK;
}
int32_t _mpack_auth_switch(mysql_params *params, binary_ctx *breader, mpack_auth_switch *auswitch) {
    auswitch->plugin = binary_get_string(breader, 0);
    auswitch->provided.lens = breader->size - breader->offset;
    auswitch->provided.data = binary_get_string(breader, auswitch->provided.lens);
    return ERR_OK;
}
int32_t _mpack_parser(mysql_params *params, binary_ctx *breader, mysql_pack_ctx *mpack) {
    int32_t rtn;
    switch (mpack->command) {
    case MYSQL_OK:
        MALLOC(mpack->mpack, sizeof(mpack_ok));
        rtn = _mpack_ok(params, breader, mpack->mpack);
        break;
    case MYSQL_EOF:
        MALLOC(mpack->mpack, sizeof(mpack_eof));
        rtn = _mpack_eof(params, breader, mpack->mpack);
        break;
    case MYSQL_ERR: 
        MALLOC(mpack->mpack, sizeof(mpack_err));
        rtn = _mpack_err(params, breader, mpack->mpack);
        break;
    default:
        rtn = ERR_FAILED;
        break;
    }
    return rtn;
}
