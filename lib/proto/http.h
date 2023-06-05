#ifndef HTTP_H_
#define HTTP_H_

#include "structs.h"
#include "buffer.h"

typedef struct http_header_ctx {
    buf_ctx key;
    buf_ctx value;
}http_header_ctx;

void *http_unpack(buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd);
void http_pkfree(void *data);
void http_udfree(ud_cxt *ud);
void *_http_parsehead(buffer_ctx *buf, int32_t *status, int32_t *closefd);
int32_t _http_check_keyval(http_header_ctx *head, const char *key, const char *val);

buf_ctx *http_status(void *data);
size_t http_nheader(void *data);
http_header_ctx *http_header_at(void *data, size_t pos);
char *http_header(void *data, const char *header, size_t *lens);
int32_t http_chunked(void *data);
void *http_data(void *data, size_t *lens);

#endif//HTTP_H_
