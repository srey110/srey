#ifndef CRC_H_
#define CRC_H_

#include "base/base.h"

/// <summary>
/// CRC-16/ARC（又称 IBM：反射、多项式 0x8005、初值 0、无输出异或）
/// </summary>
/// <param name="data">数据</param>
/// <param name="lens">数据长度</param>
/// <returns>crc 16</returns>
uint16_t crc16(const void *data, const size_t lens);
/// <summary>
/// crc-32
/// </summary>
/// <param name="data">数据</param>
/// <param name="lens">数据长度</param>
/// <returns>crc 32</returns>
uint32_t crc32(const void *data, const size_t lens);

#endif//CRC_H_
