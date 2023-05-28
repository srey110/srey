#ifndef HTTP_H_
#define HTTP_H_

#include "structs.h"
#include "buffer.h"

void *http_unpack(buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd);

#endif//HTTP_H_
