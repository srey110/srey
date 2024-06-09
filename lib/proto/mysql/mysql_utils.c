#include "proto/mysql/mysql_utils.h"
#include "proto/mysql/mysql_macro.h"

void _mysql_set_lenenc(binary_ctx *bwriter, size_t integer) {
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
uint64_t _mysql_get_lenenc(binary_ctx *breader) {
    uint8_t flag = binary_get_uint8(breader);
    if (flag <= 0xfa) {
        return flag;
    }
    if (0xfc == flag) {
        return binary_get_uinteger(breader, 2, 1);
    }
    if (0xfd == flag) {
        return binary_get_uinteger(breader, 3, 1);
    }
    if (0xfe == flag) {
        return binary_get_uinteger(breader, 8, 1);
    }
    LOG_ERROR("unknow int<lenenc>, %d.", (int32_t)flag);
    return 0;
}
void _set_payload_lens(binary_ctx *bwriter) {
    size_t size = bwriter->offset;
    binary_offset(bwriter, 0);
    binary_set_integer(bwriter, size - MYSQL_HEAD_LENS, 3, 1);
    binary_offset(bwriter, size);
}
