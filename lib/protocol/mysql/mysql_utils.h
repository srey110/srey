#ifndef MYSQL_UTILS_H_
#define MYSQL_UTILS_H_

#include "utils/binary.h"

// 内部函数：将整数以 MySQL lenenc（长度编码整数）格式写入缓冲区
void _mysql_set_lenenc(binary_ctx *bwriter, size_t integer);
// 内部函数：从缓冲区读取 MySQL lenenc 格式的整数；遇到未知 flag 时 *err 置 ERR_FAILED，否则置 ERR_OK
uint64_t _mysql_get_lenenc(binary_ctx *breader, int32_t *err);
// 内部函数：将 bwriter 偏移 0-2 字节回填为实际 payload 长度（排除 4 字节包头）
void _set_payload_lens(binary_ctx *bwriter);

#endif//MYSQL_UTILS_H_
