#ifndef MYSQL_UTILS_H_
#define MYSQL_UTILS_H_

#include "utils/binary.h"

// 内部函数：将整数以 MySQL lenenc（长度编码整数）格式写入缓冲区
void _mysql_set_lenenc(binary_ctx *bwriter, size_t integer);
// 内部函数：从缓冲区读取 MySQL lenenc 格式的整数；未知 flag 或剩余字节不足时 *err 置 ERR_FAILED，
// 否则置 ERR_OK。失败时 offset 可能已推进，调用方判到 ERR_FAILED 必须整段放弃而不是接着读
uint64_t _mysql_get_lenenc(binary_ctx *breader, int32_t *err);
// 同 _mysql_get_lenenc。列定义每包读 7 次，首字节 <= 0xfa(长度 <= 250，绝大多数)时就地读完，不走外部函数
static inline uint64_t _mysql_lenenc(binary_ctx *breader, int32_t *err) {
    if (breader->offset < breader->size
        && (uint8_t)breader->data[breader->offset] <= 0xfa) {
        *err = ERR_OK;
        return (uint8_t)breader->data[breader->offset++];
    }
    return _mysql_get_lenenc(breader, err);
}
// 内部函数：将 bwriter 偏移 0-2 字节回填为实际 payload 长度（排除 4 字节包头）；超 16MB 返 ERR_FAILED
int32_t _mysql_set_payload_lens(binary_ctx *bwriter);

#endif//MYSQL_UTILS_H_
