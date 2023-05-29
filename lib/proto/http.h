#ifndef HTTP_H_
#define HTTP_H_

#include "structs.h"
#include "buffer.h"

void *http_unpack(buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd);
void http_pkfree(void *data);
void http_exfree(void *extra);

#endif//HTTP_H_
