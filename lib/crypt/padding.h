#ifndef PADDING_H_
#define PADDING_H_

#include "base/macro.h"

typedef enum padding_model {
    NoPadding = 0x00,
    ZeroPadding,
    PKCS57,
    ISO10126,
    ANSIX923
}padding_model;
/// <summary>
/// 数据填充
/// </summary>
/// <param name="padding">padding_model</param>
/// <param name="data">需要填充的数据</param>
/// <param name="dlens">数据长度</param>
/// <param name="output">输出填充后的数据</param>
/// <param name="reqlens">要求的数据长度</param>
void _padding_data(padding_model padding, const void *data, size_t dlens, uint8_t *output, size_t reqlens);
/// <summary>
/// 密码填充
/// </summary>
/// <param name="key">密码</param>
/// <param name="klens">密码长度</param>
/// <param name="pdkey">填充密码</param>
/// <param name="reqlens">要求的密码长度</param>
/// <returns>填充后的密码</returns>
uint8_t *_padding_key(const char *key, size_t klens, uint8_t *pdkey, size_t reqlens);

#endif//PADDING_H_
