#include "protocol/mysql/mysql_utils.h"
#include "protocol/mysql/mysql_macro.h"

// 将整数以 MySQL lenenc（长度编码整数）格式写入缓冲区：
// <= 0xfa 用 1 字节；<= 0xFFFF 用 0xfc + 2 字节；<= 0xFFFFFF 用 0xfd + 3 字节；其余用 0xfe + 8 字节
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
// 从缓冲区读取 MySQL lenenc 格式的整数：
// 首字节 <= 0xfa 直接返回；0xfc 读 2 字节；0xfd 读 3 字节；0xfe 读 8 字节
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
// 将 bwriter 中偏移 0-2 字节回填为实际 payload 长度（总长度减去 4 字节包头）
void _set_payload_lens(binary_ctx *bwriter) {
    size_t size = bwriter->offset;
    binary_offset(bwriter, 0);
    binary_set_integer(bwriter, size - MYSQL_HEAD_LENS, 3, 1);
    binary_offset(bwriter, size);
}
