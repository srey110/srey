#ifndef XOR_H_
#define XOR_H_

#include "base/macro.h"

/// <summary>
/// 异或编码
/// </summary>
/// <param name="key">密钥</param>
/// <param name="round">轮数</param>
/// <param name="data">要编码的数据</param>
/// <param name="lens">数据长度</param>
/// <returns>编码后的数据</returns>
void *xor_encode(const char key[4], const size_t round, void *data, const size_t lens);
/// <summary>
/// 异或解码
/// </summary>
/// <param name="key">密钥</param>
/// <param name="round">轮数</param>
/// <param name="data">要解码的数据</param>
/// <param name="lens">数据长度</param>
/// <returns>解码后的数据</returns>
void *xor_decode(const char key[4], const size_t round, void *data, const size_t lens);

#endif//XOR_H_
