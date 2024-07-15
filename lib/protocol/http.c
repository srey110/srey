#include "protocol/http.h"
#include "protocol/protos.h"
#include "crypt/urlraw.h"
#include "containers/sarray.h"
#include "utils/utils.h"

typedef enum parse_status{
    INIT = 0,
    CONTENT,
    CHUNKED
}parse_status;
ARRAY_DECL(http_header_ctx, arr_header);
typedef struct http_pack_ctx {
    int32_t chunked;
    buf_ctx head;
    buf_ctx data;
    buf_ctx status[3];
    arr_header_ctx header;
}http_pack_ctx;

#define MAX_HEADLENS ONEK * 4
#define HEAD_REMAIN (pack->head.lens - (head - (char *)pack->head.data))

int32_t _http_check_keyval(http_header_ctx *head, const char *key, const char *val) {
    if (!buf_icompare(&head->key, key, strlen(key))) {
        return ERR_FAILED;
    }
    if (NULL == val) {
        return ERR_OK;
    }
    if (NULL != memstr(1, head->value.data, head->value.lens, val, strlen(val))) {
        return ERR_OK;
    }
    return ERR_FAILED;
}
static void _check_fileld(http_pack_ctx *pack, http_header_ctx *field, int32_t *transfer) {
    switch (tolower(*((char *)field->key.data))) {
    case 'c':
        if (ERR_OK == _http_check_keyval(field, "content-length", NULL)) {
            *transfer = CONTENT;
            pack->data.lens = strtol(field->value.data, NULL, 10);
        }
        break;
    case 't':
        if (ERR_OK == _http_check_keyval(field, "transfer-encoding", "chunked")){
            *transfer = CHUNKED;
            pack->data.lens = 0;
            pack->chunked = 1;
        }
        break;
    default:
        break;
    }
}
static char *_http_parse_status(http_pack_ctx *pack) {
    char *head = pack->head.data;
    head = skipempty(head, pack->head.lens);
    if (NULL == head) {
        return NULL;
    }
    char *pos = memstr(0, head, HEAD_REMAIN, " ", 1);
    if (NULL == pos) {
        return NULL;
    }
    pack->status[0].data = head;
    pack->status[0].lens = pos - head;
    if (0 == pack->status[0].lens) {
        return NULL;
    }
    head = pos + 1;
    pos = memstr(0, head, HEAD_REMAIN, " ", 1);
    if (NULL == pos) {
        return NULL;
    }
    pack->status[1].data = head;
    pack->status[1].lens = pos - head;
    if (0 == pack->status[1].lens) {
        return NULL;
    }
    head = pos + 1;
    pos = memstr(0, head, HEAD_REMAIN, FLAG_CRLF, CRLF_SIZE);
    if (NULL == pos) {
        return NULL;
    }
    pack->status[2].data = head;
    pack->status[2].lens = pos - head;
    return pos + CRLF_SIZE;
}
static int32_t _http_parse_head(http_pack_ctx *pack, int32_t *transfer) {
    char *head = _http_parse_status(pack);
    if (NULL == head) {
        return ERR_FAILED;
    }
    char *pos;
    size_t least = 2 * CRLF_SIZE + 1;//\r\n\r\n + :
    http_header_ctx field;
    while ((size_t)(head + least - (char *)pack->head.data) <= pack->head.lens) {
        head = skipempty(head, HEAD_REMAIN);
        if (NULL == head) {
            return ERR_FAILED;
        }
        pos = memstr(0, head, HEAD_REMAIN, ":", 1);
        if (NULL == pos) {
            return ERR_FAILED;
        }
        field.key.data = head;
        field.key.lens = pos - head;
        if (0 == field.key.lens) {
            return ERR_FAILED;
        }
        head = pos + 1;
        head = skipempty(head, HEAD_REMAIN);
        if (NULL == head) {
            return ERR_FAILED;
        }
        pos = memstr(0, head, HEAD_REMAIN, FLAG_CRLF, CRLF_SIZE);
        if (NULL == pos) {
            return ERR_FAILED;
        }
        field.value.data = head;
        field.value.lens = pos - head;
        head = pos + CRLF_SIZE;
        if (CHUNKED != *transfer) {
            _check_fileld(pack, &field, transfer);
        }
        arr_header_push_back(&pack->header, &field);
    }
    return ERR_OK;
}
static http_pack_ctx *_http_content(buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    http_pack_ctx *pack = ud->extra;
    if (buffer_size(buf) >= pack->data.lens) {
        MALLOC(pack->data.data, pack->data.lens);
        ASSERTAB(pack->data.lens == buffer_remove(buf, pack->data.data, pack->data.lens), "copy buffer failed.");
        ud->status = INIT;
        ud->extra = NULL;
        return pack;
    } else {
        BIT_SET(*status, PROTO_MOREDATA);
        return NULL;
    }
}
static size_t _http_headlens(buffer_ctx *buf, int32_t *status) {
    size_t flens = CRLF_SIZE * 2;
    int32_t pos = buffer_search(buf, 0, 0, 0, CONCAT2(FLAG_CRLF,FLAG_CRLF), flens);
    if (ERR_FAILED == pos) {
        if (buffer_size(buf) > MAX_HEADLENS) {
            BIT_SET(*status, PROTO_ERROR);
        } else {
            BIT_SET(*status, PROTO_MOREDATA);
        }
        return 0;
    }
    size_t hlens = pos + flens;
    if (hlens > MAX_HEADLENS) {
        BIT_SET(*status, PROTO_ERROR);
        return 0;
    }
    return hlens;
}
static http_pack_ctx *_http_headpack(size_t lens) {
    char *pack;
    CALLOC(pack, 1, sizeof(http_pack_ctx) + lens);
    ((http_pack_ctx *)pack)->head.data = pack + sizeof(http_pack_ctx);
    ((http_pack_ctx *)pack)->head.lens = lens;
    arr_header_init(&((http_pack_ctx *)pack)->header, 0);
    return (http_pack_ctx *)pack;
}
http_pack_ctx *_http_parsehead(buffer_ctx *buf, int32_t *transfer, int32_t *status) {
    size_t hlens = _http_headlens(buf, status);
    if (0 == hlens) {
        return NULL;
    }
    *transfer = 0;
    http_pack_ctx *pack = _http_headpack(hlens);
    ASSERTAB(hlens == buffer_remove(buf, pack->head.data, hlens), "copy buffer failed.");
    if (ERR_OK != _http_parse_head(pack, transfer)) {
        BIT_SET(*status, PROTO_ERROR);
        _http_pkfree(pack);
        return NULL;
    }
    return pack;
}
static http_pack_ctx *_http_header(buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    int32_t transfer;
    http_pack_ctx *pack = _http_parsehead(buf, &transfer, status);
    if (NULL == pack) {
        return NULL;
    }
    if (CONTENT == transfer) {
        if (PACK_TOO_LONG(pack->data.lens)) {
            BIT_SET(*status, PROTO_ERROR);
            _http_pkfree(pack);
            return NULL;
        } else {
            ud->extra = pack;
            ud->status = transfer;
            return _http_content(buf, ud, status);
        }
    } else {
        if (1 == pack->chunked) {
            BIT_SET(*status, PROTO_SLICE_START);
        }
        ud->status = transfer;
        return pack;
    }
}
static http_pack_ctx *_http_chunkedpack(size_t lens) {
    char *pack;
    CALLOC(pack, 1, sizeof(http_pack_ctx) + lens);
    http_pack_ctx *pctx = (http_pack_ctx *)pack;
    if (lens > 0) {
        pctx->data.data = pack + sizeof(http_pack_ctx);
        pctx->data.lens = lens;
    }
    pctx->chunked = 2;
    return pctx;
}
static http_pack_ctx *_http_chunked(buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t drain;
    http_pack_ctx *pack = ud->extra;
    if (NULL == pack) {
        int32_t pos = buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE);
        if (ERR_FAILED == pos) {
            BIT_SET(*status, PROTO_MOREDATA);
            return NULL;
        }
        char lensbuf[16] = { 0 };
        if (pos >= sizeof(lensbuf)) {
            BIT_SET(*status, PROTO_ERROR);
            return NULL;
        }
        ASSERTAB(pos == buffer_copyout(buf, 0, lensbuf, pos), "copy buffer failed.");
        size_t dlens = strtol(lensbuf, NULL, 16);
        if (PACK_TOO_LONG(dlens)) {
            BIT_SET(*status, PROTO_ERROR);
            return NULL;
        }
        drain = pos + CRLF_SIZE;
        ASSERTAB(drain == buffer_drain(buf, drain), "drain buffer failed.");
        pack = _http_chunkedpack(dlens);
        ud->extra = pack;
    }
    drain = pack->data.lens + CRLF_SIZE;
    if (buffer_size(buf) < drain) {
        BIT_SET(*status, PROTO_MOREDATA);
        return NULL;
    }
    if (pack->data.lens > 0) {
        BIT_SET(*status, PROTO_SLICE);
        ASSERTAB(pack->data.lens == buffer_copyout(buf, 0, pack->data.data, pack->data.lens), "copy buffer failed.");
    } else {
        BIT_SET(*status, PROTO_SLICE_END);
        ud->status = INIT;
    }
    ASSERTAB(drain == buffer_drain(buf, drain), "drain buffer failed.");
    ud->extra = NULL;
    return pack;
}
void _http_pkfree(http_pack_ctx *pack) {
    if (NULL == pack) {
        return;
    }
    if (NULL != pack->head.data) {
        FREE(pack->data.data);
        arr_header_free(&pack->header);
    }
    FREE(pack);
}
void _http_udfree(ud_cxt *ud) {
    _http_pkfree(ud->extra);
    ud->extra = NULL;
}
http_pack_ctx *http_unpack(buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    http_pack_ctx *pack;
    switch (ud->status) {
    case INIT:
        pack = _http_header(buf, ud, status);
        break;
    case CONTENT:
        pack = _http_content(buf, ud, status);
        break;
    case CHUNKED:
        pack = _http_chunked(buf, ud, status);
        break;
    default:
        pack = NULL;
        BIT_SET(*status, PROTO_ERROR);
        break;
    }
    return pack;
}
buf_ctx *http_status(http_pack_ctx *pack) {
    return pack->status;
}
uint32_t http_nheader(http_pack_ctx *pack) {
    return arr_header_size(&pack->header);
}
http_header_ctx *http_header_at(http_pack_ctx *pack, uint32_t pos) {
    return arr_header_at(&pack->header, pos);
}
char *http_header(http_pack_ctx *pack, const char *header, size_t *lens) {
    if (NULL == pack->head.data) {
        return NULL;
    }
    http_header_ctx *filed;
    size_t klens = strlen(header);
    uint32_t n = arr_header_size(&pack->header);
    for (uint32_t i = 0; i < n; i++) {
        filed = arr_header_at(&pack->header, i);
        if (buf_icompare(&filed->key, header, klens)) {
            *lens = filed->value.lens;
            return filed->value.data;
        }
    }
    return NULL;
}
int32_t http_chunked(http_pack_ctx *pack) {
    return pack->chunked;
}
void *http_data(http_pack_ctx *pack, size_t *lens) {
    *lens = pack->data.lens;
    return pack->data.data;
}
void http_pack_req(binary_ctx *bwriter, const char *method, const char *url) {
    binary_set_va(bwriter, "%s %s HTTP/1.1"FLAG_CRLF, method, url);
}
const char *http_code_status(int32_t code) {
    switch (code) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 305: return "Use Proxy";
    case 307: return "Temporary Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Time-out";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Request Entity Too Large";
    case 414: return "Request-URI Too Large";
    case 415: return "Unsupported Media Type";
    case 416: return "Requested range not satisfiable";
    case 417: return "Expectation Failed";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Time-out";
    case 505: return "HTTP Version not supported";
    default:
        return "Unknown";
    }
}
void http_pack_resp(binary_ctx *bwriter, int32_t code) {
    binary_set_va(bwriter, "HTTP/1.1 %d %s"FLAG_CRLF, code, http_code_status(code));
}
void http_pack_head(binary_ctx *bwriter, const char *key, const char *val) {
    binary_set_va(bwriter, "%s: %s"FLAG_CRLF, key, val);
}
void http_pack_end(binary_ctx *bwriter) {
    binary_set_va(bwriter, FLAG_CRLF, CRLF_SIZE);
}
void http_pack_content(binary_ctx *bwriter, void *data, size_t lens) {
    if (!EMPTYSTR(data)) {
        binary_set_va(bwriter, "Content-Length: %d"CONCAT2(FLAG_CRLF, FLAG_CRLF), (uint32_t)lens);
        binary_set_string(bwriter, data, lens);
    } else {
        binary_set_va(bwriter, "%s", "Content-Length: 0"CONCAT2(FLAG_CRLF, FLAG_CRLF));
    }
}
void http_pack_chunked(binary_ctx *bwriter, void *data, size_t lens) {
    if (bwriter->offset > 0){
        binary_set_va(bwriter, "Transfer-Encoding: Chunked"CONCAT2(FLAG_CRLF, FLAG_CRLF));
    }
    binary_set_va(bwriter, "%x"FLAG_CRLF, (uint32_t)lens);
    if (lens > 0) {
        binary_set_string(bwriter, data, lens);
    }
    binary_set_string(bwriter, FLAG_CRLF, CRLF_SIZE);
}
