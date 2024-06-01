#include "proto/mysql_parse.h"
#include "proto/mysql_macro.h"
#include "utils.h"

static void _mysql_parse_fixed_lens_int(binary_ctx *binary, uint64_t *size) {
    uint8_t flag = binary_get_uint8(binary);
    if (flag >= 0 && flag <= 0xfa) {
        *size = flag;
    } else if (0xfc == flag) {
        *size = (uint64_t)binary_get_uint16(binary, 2, 1);
    } else if (0xfd == flag) {
        *size = (uint64_t)binary_get_uint32(binary, 3, 1);
    } else if (0xfe == flag) {
        *size = binary_get_uint64(binary, 8, 1);
    } else {
        *size = 0;
    }
}
int32_t _mpack_ok(mysql_params *params, binary_ctx *breader, mpack_ok *ok) {
    uint64_t size;
    _mysql_parse_fixed_lens_int(breader, &size);
    ok->affected_rows = (int64_t)size;
    _mysql_parse_fixed_lens_int(breader, &size);
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
    size_t lens = breader->size - breader->offset;
    char *buf = binary_get_string(breader, lens);
    lens = (lens <= sizeof(err->error_msg) - 1) ? lens : (sizeof(err->error_msg) - 1);
    memcpy(err->error_msg, buf, lens);
    err->error_msg[lens] = '\0';
    return ERR_OK;
}
int32_t _mpack_auth_switch(mysql_params *params, binary_ctx *breader, mpack_auth_switch *auswitch) {
    auswitch->plugin = binary_get_string(breader, 0);
    auswitch->provided.lens = breader->size - breader->offset;
    auswitch->provided.data = binary_get_string(breader, auswitch->provided.lens);
    return ERR_OK;
}
int32_t _mpack_parser(mysql_params *params, binary_ctx *breader, mysql_pack_ctx *pk) {
    pk->command = binary_get_uint8(breader);
    switch (pk->command) {
    case MYSQL_PACK_OK:
    case MYSQL_PACK_EOF:
        
        break;
    case MYSQL_PACK_ERR:
        break;
    default:
        break;
    }
    return ERR_OK;
}
