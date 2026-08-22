#ifndef BASE64_H_
#define BASE64_H_

#include "base/macro.h"

#define B64EN_SIZE(s)   ((((s) + 2) / 3 * 4) + 1)
#define B64DE_SIZE(s)   ((((s) + 3) / 4 * 3) + 1)
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
/// <param name="out">解码后的数据,预估长度:B64DE_SIZE(lens)；末尾补 '\0'，
/// 故须能装下 解码长度 + 1 字节（B64DE_SIZE 里那个 +1 就是给它的）</param>
/// <returns>解码后的数据长度；输入非法（'=' 后有正文、非 base64 字符、尾组只剩 1 个字符）返回 0，
/// 此时 out[0] 已置 '\0'</returns>
size_t bs64_decode(const char *data, const size_t lens, char *out);

#endif//BASE64_H_
