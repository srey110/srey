#ifndef URLRAW_H_
#define URLRAW_H_

#include "base/macro.h"

#define URLEN_SIZE(s) (3 * (s) + 1)
/// <summary>
/// URL编码
/// </summary>
/// <param name="data">要编码的数据</param>
/// <param name="lens">数据长度</param>
/// <param name="out">编码后的数据, 长度:URLEN_SIZE(lens)</param>
/// <param name="space2plus">非 0:空格编码为 '+'(application/x-www-form-urlencoded)；0:空格编码为 %20(RFC 3986)</param>
/// <returns>编码后的数据</returns>
char *url_encode(const char *data, const size_t lens, char *out, int32_t space2plus);
/// <summary>
/// URL解码
/// </summary>
/// <param name="data">要解码的数据(原地解码)；仅在 [0,lens) 内就地缩短，不写末尾 '\0'，调用方按返回长度取结果</param>
/// <param name="lens">数据长度</param>
/// <param name="plus2space">非 0:'+' 解码为空格(application/x-www-form-urlencoded)；0:'+' 保持字面(RFC 3986)</param>
/// <returns>解码后的长度</returns>
size_t url_decode(char *data, size_t lens, int32_t plus2space);

#endif//URLRAW_H_
