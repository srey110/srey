#ifndef VARINT_H_
#define VARINT_H_

#include "base/structs.h"
#include "utils/buffer.h"

/// <summary>
/// 7-bit varint 编码（MQTT 可变长度风格，每字节低 7 位为数据、最高位为延续标志）。
/// 上限 268435455（256MB-1），与 4 字节头表达上界对齐。
/// </summary>
/// <param name="value">待编码整数；value ≥ 0x10000000 视为溢出</param>
/// <param name="buf">输出缓冲，至少 4 字节</param>
/// <returns>编码占用字节数（1-4）；溢出返回 0</returns>
int32_t varint_encode_mqtt(uint32_t value, char buf[4]);
/// <summary>
/// 7-bit varint 解码：从 buf 的 off 起读，最多 4 字节，每字节低 7 位累加、最高位为延续标志。
/// </summary>
/// <param name="buf">输入缓冲</param>
/// <param name="off">起始偏移</param>
/// <param name="blens">可读字节上限</param>
/// <param name="value">输出解码值</param>
/// <returns>占用字节数（1-4）；可读字节不足或越 4 字节未结束返回 ERR_FAILED</returns>
int32_t varint_decode_mqtt(buffer_ctx *buf, size_t off, size_t blens, size_t *value);

#endif
