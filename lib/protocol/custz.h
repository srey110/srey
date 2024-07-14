#ifndef CUSTOMIZE_H_
#define CUSTOMIZE_H_

#include "base/structs.h"
#include "utils/buffer.h"

//解包
struct custz_pack_ctx *custz_unpack(buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);
/// <summary>
/// 组包
/// </summary>
/// <param name="data">数据</param>
/// <param name="lens">数据长度</param>
/// <param name="size">组包后的数据长度</param>
/// <returns>custz_pack_ctx</returns>
struct custz_pack_ctx *custz_pack(void *data, size_t lens, size_t *size);
/// <summary>
/// 获取数据
/// </summary>
/// <param name="pack">custz_pack_ctx</param>
/// <param name="lens">数据长度</param>
/// <returns>数据</returns>
void *custz_data(struct custz_pack_ctx *pack, size_t *lens);

#endif//CUSTOMIZE_H_
