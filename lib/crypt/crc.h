#ifndef CRC_H_
#define CRC_H_

#include "base/macro.h"

/// <summary>
/// crc 16 IBM
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
