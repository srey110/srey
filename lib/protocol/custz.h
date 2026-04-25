#ifndef CUSTOMIZE_H_
#define CUSTOMIZE_H_

#include "utils/buffer.h"

/// <summary>
/// 自定义协议解包，根据 pktype 选择固定长度/标志位/变长编码方式
/// </summary>
/// <param name="pktype">包类型，取值见 pack_type（PACK_CUSTZ_FIXED / PACK_CUSTZ_FLAG / PACK_CUSTZ_VAR）</param>
/// <param name="buf">接收缓冲区</param>
/// <param name="size">输出：解包后数据体长度</param>
/// <param name="status">输出：解包状态标志，见 prot_status</param>
/// <returns>解包后的数据指针（调用方负责释放），数据不足或出错时返回 NULL</returns>
void *custz_unpack(uint8_t pktype, buffer_ctx *buf, size_t *size, int32_t *status);
/// <summary>
/// 自定义协议组包，根据 pktype 选择固定长度/标志位/变长编码方式
/// </summary>
/// <param name="pktype">包类型，取值见 pack_type（PACK_CUSTZ_FIXED / PACK_CUSTZ_FLAG / PACK_CUSTZ_VAR）</param>
/// <param name="data">待打包的数据</param>
/// <param name="lens">数据长度</param>
/// <param name="size">输出：组包后总长度（头部 + 数据）</param>
/// <returns>组好的包（调用方负责释放）</returns>
void *custz_pack(uint8_t pktype, void *data, size_t lens, size_t *size);

#endif//CUSTOMIZE_H_
