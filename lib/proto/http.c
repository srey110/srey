#include "http.h"
#include "utils.h"
#include "sarray.h"
#include "loger.h"

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
    arr_header header;
}http_pack_ctx;

#define MAX_HEADLENS ONEK * 4
#define FLAG_CRLF "\r\n"
#define FLAG_HEND "\r\n\r\n"
#define FLAG_CONTENT "content-length"
#define FLAG_CHUNKED "transfer-encoding"
#define CHUNKED_KEY "chunked"
#define HEAD_REMAIN (pack->head.lens - (head - (char *)pack->head.data))

int32_t _http_check_keyval(http_header_ctx *head, const char *key, const char *val) {
    size_t lens = strlen(key);
    if (head->key.lens < lens
        || 0 != _memicmp(head->key.data, key, lens)) {
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
static inline void _check_fileld(http_pack_ctx *pack, http_header_ctx *field, int32_t *status) {
    switch (tolower(*((char *)field->key.data))) {
    case 'c':
        if (ERR_OK == _http_check_keyval(field, FLAG_CONTENT, NULL)) {
            *status = CONTENT;
            pack->data.lens = atoi(field->value.data);
        }
        break;
    case 't':
        if (ERR_OK == _http_check_keyval(field, FLAG_CHUNKED, CHUNKED_KEY)){
            *status = CHUNKED;
            pack->data.lens = 0;
            pack->chunked = 1;
        }
        break;
    default:
        break;
    }
}
static inline char *_http_parse_status(http_pack_ctx *pack, size_t flens) {
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
    if (0 != _memicmp(pack->status[0].data, "http", strlen("http"))) {
        pack->status[1].lens = urldecode(pack->status[1].data, pack->status[1].lens);
    }
    pos = memstr(0, head, HEAD_REMAIN, FLAG_CRLF, flens);
    if (NULL == pos) {
        return NULL;
    }
    pack->status[2].data = head;
    pack->status[2].lens = pos - head;
    return pos + flens;
}
static inline int32_t _http_parse_head(http_pack_ctx *pack, int32_t *status) {
    size_t flens = strlen(FLAG_CRLF);
    char *head = _http_parse_status(pack, flens);
    if (NULL == head) {
        return ERR_FAILED;
    }
    char *pos;
    size_t least = 2 * flens + 1;//\r\n\r\n + :
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
        pos = memstr(0, head, HEAD_REMAIN, FLAG_CRLF, flens);
        if (NULL == pos) {
            return ERR_FAILED;
        }
        field.value.data = head;
        field.value.lens = pos - head;
        head = pos + flens;
        if (CHUNKED != *status) {
            _check_fileld(pack, &field, status);
        }
        arr_header_push_back(&pack->header, &field);
    }
    return ERR_OK;
}
static inline void *_http_content(buffer_ctx *buf, ud_cxt *ud, int32_t *closefd) {
    http_pack_ctx *pack = ud->extra;
    if (buffer_size(buf) >= pack->data.lens) {
        MALLOC(pack->data.data, pack->data.lens);
        ASSERTAB(pack->data.lens == buffer_remove(buf, pack->data.data, pack->data.lens), "copy buffer failed.");
        ud->status = INIT;
        ud->extra = NULL;
        return pack;
    } else {
        return NULL;
    }
}
static inline size_t _http_headlens(buffer_ctx *buf, int32_t *closefd) {
    size_t flens = strlen(FLAG_HEND);
    int32_t pos = buffer_search(buf, 0, 0, 0, FLAG_HEND, flens);
    if (ERR_FAILED == pos) {
        if (buffer_size(buf) > MAX_HEADLENS) {
            *closefd = 1;
            LOG_WARN("%s", "http head too long.");
        }
        return 0;
    }
    size_t hlens = pos + flens;
    if (hlens > MAX_HEADLENS) {
        *closefd = 1;
        LOG_WARN("http head too long, %"PRIu64, hlens);
        return 0;
    }
    return hlens;
}
static inline http_pack_ctx *_http_headpack(size_t lens) {
    char *pack;
    CALLOC(pack, 1, sizeof(http_pack_ctx) + lens);
    ((http_pack_ctx *)pack)->head.data = pack + sizeof(http_pack_ctx);
    ((http_pack_ctx *)pack)->head.lens = lens;
    arr_header_init(&((http_pack_ctx *)pack)->header, ARRAY_INIT_SIZE);
    return (http_pack_ctx *)pack;
}
void *_http_parsehead(buffer_ctx *buf, int32_t *status, int32_t *closefd) {
    size_t hlens = _http_headlens(buf, closefd);
    if (0 == hlens) {
        return NULL;
    }
    *status = 0;
    http_pack_ctx *pack = _http_headpack(hlens);
    ASSERTAB(hlens == buffer_remove(buf, pack->head.data, hlens), "copy buffer failed.");
    if (ERR_OK != _http_parse_head(pack, status)) {
        *closefd = 1;
        LOG_NOEOFSTR(LOGLV_WARN, "http parse head failed.\n%s", pack->head.data, pack->head.lens);
        http_pkfree(pack);
        return NULL;
    }
    return pack;
}
static inline void *_http_header(buffer_ctx *buf, ud_cxt *ud, int32_t *closefd) {
    int32_t status;
    http_pack_ctx *pack = _http_parsehead(buf, &status, closefd);
    if (NULL == pack) {
        return NULL;
    }
    if (CONTENT == status) {
        if (PACK_TOO_LONG(pack->data.lens)) {
            *closefd = 1;
            http_pkfree(pack);
            LOG_WARN("http data too long, %"PRIu64, pack->data.lens);
            return NULL;
        } else {
            ud->extra = pack;
            ud->status = status;
            return _http_content(buf, ud, closefd);
        }
    } else {
        ud->status = status;
        return pack;
    }
}
static inline http_pack_ctx *_http_chunkedpack(size_t lens) {
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
static inline void *_http_chunked(buffer_ctx *buf, ud_cxt *ud, int32_t *closefd) {
    size_t drain;
    size_t flens = strlen(FLAG_CRLF);
    http_pack_ctx *pack = ud->extra;
    if (NULL == pack) {
        int32_t pos = buffer_search(buf, 0, 0, 0, FLAG_CRLF, flens);
        if (ERR_FAILED == pos) {
            return NULL;
        }
        char lensbuf[16] = { 0 };
        if (pos >= sizeof(lensbuf)) {
            *closefd = 1;
            LOG_WARN("data lens too long. string lens: %d", pos);
            return NULL;
        }
        ASSERTAB(pos == buffer_copyout(buf, 0, lensbuf, pos), "copy buffer failed.");
        size_t dlens = atoi(lensbuf);
        if (PACK_TOO_LONG(dlens)) {
            *closefd = 1;
            LOG_WARN("data too long, lens:%"PRIu64, dlens);
            return NULL;
        }
        drain = pos + flens;
        ASSERTAB(drain == buffer_drain(buf, drain), "drain buffer failed.");
        pack = _http_chunkedpack(dlens);
        ud->extra = pack;
    }
    drain = pack->data.lens + flens;
    if (buffer_size(buf) < drain) {
        return NULL;
    }
    if (pack->data.lens > 0) {
        ASSERTAB(pack->data.lens == buffer_copyout(buf, 0, pack->data.data, pack->data.lens), "copy buffer failed.");
    } else {
        ud->status = INIT;
    }
    ASSERTAB(drain == buffer_drain(buf, drain), "drain buffer failed.");
    ud->extra = NULL;
    return pack;
}
void http_pkfree(void *data) {
    if (NULL == data) {
        return;
    }
    http_pack_ctx *pack = data;
    if (NULL != pack->head.data) {
        FREE(pack->data.data);
        arr_header_free(&pack->header);
    }
    FREE(pack);
}
void http_udfree(ud_cxt *ud) {
    http_pkfree(ud->extra);
}
void *http_unpack(buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd) {
    void *data;
    switch (ud->status) {
    case INIT:
        data = _http_header(buf, ud, closefd);
        break;
    case CONTENT:
        data = _http_content(buf, ud, closefd);
        break;
    case CHUNKED:
        data = _http_chunked(buf, ud, closefd);
        break;
    default:
        data = NULL;
        *closefd = 1;
        break;
    }
    return data;
}
buf_ctx *http_status(void *data) {
    return ((http_pack_ctx *)data)->status;
}
size_t http_nheader(void *data) {
    return arr_header_size(&((http_pack_ctx *)data)->header);
}
http_header_ctx *http_header_at(void *data, size_t pos) {
    return arr_header_at(&((http_pack_ctx *)data)->header, pos);
}
char *http_header(void *data, const char *header, size_t *lens) {
    http_pack_ctx *pack = data;
    if (NULL == pack->head.data) {
        return NULL;
    }
    http_header_ctx *filed;
    size_t klens = strlen(header);
    for (size_t i = 0; i < arr_header_size(&pack->header); i++) {
        filed = arr_header_at(&pack->header, i);
        if (filed->key.lens >= klens
            && 0 == _memicmp(filed->key.data, header, klens)) {
            *lens = filed->value.lens;
            return filed->value.data;
        }
    }
    return NULL;
}
int32_t http_chunked(void *data) {
    return ((http_pack_ctx *)data)->chunked;
}
void *http_data(void *data, size_t *lens) {
    *lens = ((http_pack_ctx *)data)->data.lens;
    return ((http_pack_ctx *)data)->data.data;
}
