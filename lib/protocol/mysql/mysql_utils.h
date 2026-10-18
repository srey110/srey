#ifndef MYSQL_UTILS_H_
#define MYSQL_UTILS_H_

#include "protocol/mysql/mysql_struct.h"

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
// 内部函数：将 bwriter 偏移 0-2 字节回填为实际 payload 长度（排除 4 字节包头）；payload 达到 INT3_MAX 返 ERR_FAILED
int32_t _mysql_set_payload_lens(binary_ctx *bwriter);
// 内部函数：组包前导。按 hint 初始化 bwriter，占 3 字节长度位(收尾由 _mysql_set_payload_lens 回填)，再写包序号。
// cmd 传命令字节(COM_xxx，都非 0)是命令首包：序号归零，序号后写 cmd；传 0 是认证阶段的后续包：序号自增，不写命令字节
void _mysql_pack_begin(mysql_ctx *mysql, binary_ctx *bwriter, size_t hint, uint8_t cmd);

#endif//MYSQL_UTILS_H_
