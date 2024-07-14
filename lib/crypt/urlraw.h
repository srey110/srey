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
/// <returns>编码后的数据</returns>
char *url_encode(const char *data, const size_t lens, char *out);
/// <summary>
/// URL解码
/// </summary>
/// <param name="data">要解码的数据</param>
/// <param name="lens">数据长度</param>
/// <returns>解码后的长度</returns>
size_t url_decode(char *data, size_t lens);

#endif//URLRAW_H_
