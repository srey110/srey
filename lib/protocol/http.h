#ifndef HTTP_H_
#define HTTP_H_

#include "base/structs.h"
#include "utils/buffer.h"
#include "utils/binary.h"

typedef struct http_header_ctx {
    buf_ctx key;
    buf_ctx value;
}http_header_ctx;
struct http_pack_ctx;

void _http_pkfree(struct http_pack_ctx *pack);
void _http_udfree(ud_cxt *ud);
struct http_pack_ctx *http_unpack(buffer_ctx *buf, ud_cxt *ud, int32_t *status);

const char *http_code_status(int32_t code);
void http_pack_req(binary_ctx *bwriter, const char *method, const char *url);
void http_pack_resp(binary_ctx *bwriter, int32_t code);
void http_pack_head(binary_ctx *bwriter, const char *key, const char *val);
void http_pack_end(binary_ctx *bwriter);//只有头的时候
void http_pack_content(binary_ctx *bwriter, void *data, size_t lens);
void http_pack_chunked(binary_ctx *bwriter, void *data, size_t lens);//循环 binary_offset send

struct http_pack_ctx *_http_parsehead(buffer_ctx *buf, int32_t *transfer, int32_t *status);
int32_t _http_check_keyval(http_header_ctx *head, const char *key, const char *val);

buf_ctx *http_status(struct http_pack_ctx *pack);
uint32_t http_nheader(struct http_pack_ctx *pack);
http_header_ctx *http_header_at(struct http_pack_ctx *pack, uint32_t pos);
char *http_header(struct http_pack_ctx *pack, const char *header, size_t *lens);
int32_t http_chunked(struct http_pack_ctx *pack);
void *http_data(struct http_pack_ctx *pack, size_t *lens);

#endif//HTTP_H_
