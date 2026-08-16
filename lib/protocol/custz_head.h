#ifndef CUSTZ_HEAD_H_
#define CUSTZ_HEAD_H_

#include "utils/buffer.h"

// 三个 _custz_encode_* 共用一条返回约定：失败返 NULL 且把 *size 置 0，*hlens 不保证有意义。
// 置 0 是为了让"忘了判 NULL 就拿 *size 去 ev_send"退化成发 0 字节而不是发一个未定值

// 固定 4 字节长度头解码：从缓冲区读取 4 字节大端整数作为数据体长度
int32_t _custz_decode_fixed(buffer_ctx *buf, size_t *hlens, size_t *size, int32_t *status);
// 固定 4 字节长度头编码：分配头部+数据体内存并写入长度字段；dlens 超 UINT32_MAX 返回 NULL
char *_custz_encode_fixed(size_t dlens, size_t *hlens, size_t *size);

// 标志位变长头解码：首字节决定后续长度字段宽度（0xfc/0xfd/0xfe/0xff）
int32_t _custz_decode_flag(buffer_ctx *buf, size_t *hlens, size_t *size, int32_t *status);
// 标志位变长头编码：按数据长度自动选择 1/3/5/9 字节头部；头长加 dlens 溢出 size_t 返回 NULL
char *_custz_encode_flag(size_t dlens, size_t *hlens, size_t *size);

// MQTT 风格变长头解码：每字节低 7 位存长度，最高位为延续标志，最多 4 字节
int32_t _custz_decode_variable(buffer_ctx *buf, size_t *hlens, size_t *size, int32_t *status);
// MQTT 风格变长头编码：按数据长度写入变长字节序列；dlens 超 268435455 返回 NULL
char *_custz_encode_variable(size_t dlens, size_t *hlens, size_t *size);

#endif//CUSTZ_HEAD_H_
