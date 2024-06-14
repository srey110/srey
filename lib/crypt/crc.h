#ifndef CRC_H_
#define CRC_H_

#include "base/macro.h"

//crc 16 IBM
uint16_t crc16(const void *data, const size_t lens);
//crc-32
uint32_t crc32(const void *data, const size_t lens);

#endif//CRC_H_
