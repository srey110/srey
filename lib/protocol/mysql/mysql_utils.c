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
// 首字节 <= 0xfa 直接返回；0xfc 读 2 字节；0xfd 读 3 字节；0xfe 读 8 字节。
// 字节不够先返 ERR_FAILED，不能让 binary_get_* 的断言 abort 整个进程：报文长度由对端决定，
// 一个截断的包只该判失败。失败时 offset 可能已推进，调用方判到 ERR_FAILED 就必须整段放弃
uint64_t _mysql_get_lenenc(binary_ctx *breader, int32_t *err) {
    *err = ERR_FAILED;
    size_t remain = binary_remain(breader);
    if (remain < sizeof(uint8_t)) {
        return 0;
    }
    uint8_t flag = binary_get_uint8(breader);
    if (flag <= 0xfa) {
        *err = ERR_OK;
        return flag;
    }
    size_t need;
    if (0xfc == flag) {
        need = 2;
    } else if (0xfd == flag) {
        need = 3;
    } else if (0xfe == flag) {
        need = 8;
    } else {
        LOG_ERROR("unknow int<lenenc>, %d.", (int32_t)flag);
        return 0;
    }
    if (remain - sizeof(uint8_t) < need) {
        return 0;
    }
    *err = ERR_OK;
    return binary_get_uinteger(breader, need, 1);
}
// 将 bwriter 中偏移 0-2 字节回填为实际 payload 长度（总长度减去 4 字节包头）
// 超 INT3_MAX(16MB-1) 时 LOG_WARN + 返 ERR_FAILED 由调用方释放 bwriter,本实现不支持 mysql 协议拆 packet
int32_t _mysql_set_payload_lens(binary_ctx *bwriter) {
    size_t size = bwriter->offset;
    size_t payload = size - MYSQL_HEAD_LENS;
    if (payload >= INT3_MAX) {
        LOG_WARN("mysql payload exceeds 16MB: %zu bytes.", payload);
        return ERR_FAILED;
    }
    binary_offset(bwriter, 0);
    binary_set_integer(bwriter, payload, 3, 1);
    binary_offset(bwriter, size);
    return ERR_OK;
}
