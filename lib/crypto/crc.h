#ifndef CRC_H_
#define CRC_H_

#include "macro.h"

//crc 16 IBM
uint16_t crc16(const char *buf, const size_t len);
uint32_t crc32(const char *buf, const size_t len);

#endif//CRC_H_
