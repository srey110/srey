#ifndef MYSQL_UTILS_H_
#define MYSQL_UTILS_H_

#include "utils/binary.h"

void _mysql_set_lenenc(binary_ctx *bwriter, size_t integer);
uint64_t _mysql_get_lenenc(binary_ctx *breader);
void _set_payload_lens(binary_ctx *bwriter);

#endif//MYSQL_UTILS_H_
