#ifndef BASE64_H_
#define BASE64_H_

#include "base/macro.h"

#define B64EN_SIZE(s)   ((((s) + 2) / 3 * 4) + 1)
#define B64DE_SIZE(s)   (((s) / 4 * 3) + 1)
/// <summary>
/// base64 编码
/// </summary>
/// <param name="data">要编码的数据</param>
/// <param name="lens">数据长度</param>
/// <param name="out">编码后的数据,预估长度:B64EN_SIZE(lens)</param>
/// <returns>编码后的数据长度</returns>
size_t bs64_encode(const void *data, const size_t lens, char *out);
/// <summary>
/// base64 解码
/// </summary>
/// <param name="data">要解码的数据</param>
/// <param name="lens">数据长度</param>
/// <param name="out">解码后的数据,预估长度:B64DE_SIZE(lens)</param>
/// <returns>解码后的数据长度</returns>
size_t bs64_decode(const char *data, const size_t lens, char *out);

#endif//BASE64_H_
