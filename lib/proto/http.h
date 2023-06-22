#ifndef HTTP_H_
#define HTTP_H_

#include "structs.h"
#include "buffer.h"

typedef struct http_header_ctx {
    buf_ctx key;
    buf_ctx value;
}http_header_ctx;

struct http_pack_ctx *http_unpack(buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd);
void http_pkfree(struct http_pack_ctx *pack);
void http_udfree(ud_cxt *ud);
struct http_pack_ctx *_http_parsehead(buffer_ctx *buf, int32_t *status, int32_t *closefd);
int32_t _http_check_keyval(http_header_ctx *head, const char *key, const char *val);

buf_ctx *http_status(struct http_pack_ctx *pack);
size_t http_nheader(struct http_pack_ctx *pack);
http_header_ctx *http_header_at(struct http_pack_ctx *pack, size_t pos);
char *http_header(struct http_pack_ctx *pack, const char *header, size_t *lens);
int32_t http_chunked(struct http_pack_ctx *pack);
void *http_data(struct http_pack_ctx *pack, size_t *lens);

#endif//HTTP_H_
