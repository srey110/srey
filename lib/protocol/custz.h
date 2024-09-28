#ifndef CUSTOMIZE_H_
#define CUSTOMIZE_H_

#include "utils/buffer.h"

//解包
void *custz_unpack(buffer_ctx *buf, size_t *size, int32_t *status);
/// <summary>
/// 组包
/// </summary>
/// <param name="data">数据</param>
/// <param name="lens">数据长度</param>
/// <param name="size">组包后的数据长度</param>
/// <returns>void *</returns>
void *custz_pack(void *data, size_t lens, size_t *size);

#endif//CUSTOMIZE_H_
