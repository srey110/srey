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
    char *status1;
    char *status2;
    char *status3;
    char *hdata;
    char *data;
    size_t slens1;
    size_t slens2;
    size_t slens3;
    size_t hlens;
    size_t lens;
    arr_header header;
}http_pack_ctx;

#define MAX_HEADLENS ONEK * 4
#define FLAG_CRLF "\r\n"
#define FLAG_HEND "\r\n\r\n"
#define FLAG_CONTENT "content-length"
#define FLAG_CHUNKED "transfer-encoding"
#define CHUNKED_KEY "chunked"

int32_t _http_check_keyval(http_header_ctx *head, const char *key, const char *val) {
    size_t lens = strlen(key);
    if (head->klen < lens
        || 0 != _memicmp(head->key, key, lens)) {
        return ERR_FAILED;
    }
    if (NULL == val) {
        return ERR_OK;
    }
    if (NULL != memstr(1, head->value, head->vlen, val, strlen(val))) {
        return ERR_OK;
    }
    return ERR_FAILED;
}
static inline void _check_fileld(http_pack_ctx *pack, http_header_ctx *field, int32_t *status) {
    switch (tolower(*(field->key))) {
    case 'c':
        if (ERR_OK == _http_check_keyval(field, FLAG_CONTENT, NULL)) {
            *status = CONTENT;
            pack->lens = atoi(field->value);
        }
        break;
    case 't':
        if (ERR_OK == _http_check_keyval(field, FLAG_CHUNKED, CHUNKED_KEY)){
            *status = CHUNKED;
            pack->lens = 0;
            pack->chunked = 1;
        }
        break;
    default:
        break;
    }
}
static inline int32_t _http_parse_head(http_pack_ctx *pack, int32_t *status) {
    //至少有一个\r\n\r\n
    char *head = pack->hdata;
    size_t flens = strlen(FLAG_CRLF);
    head = skipempty(head, pack->hlens);
    if (NULL == head) {
        return ERR_FAILED;
    }
    char *pos = memstr(0, head, pack->hlens - (head - pack->hdata), FLAG_CRLF, flens);
    if (NULL == pos) {
        return ERR_FAILED;
    }
    pack->first = head;
    pack->flens = pos - head;

    head = pos + flens;
    size_t least = 2 * flens + 1;//\r\n\r\n + :
    http_header_ctx field;
    while ((size_t)(head + least - pack->hdata) <= pack->hlens) {
        head = skipempty(head, pack->hlens - (head - pack->hdata));
        if (NULL == head) {
            return ERR_FAILED;
        }
        pos = memstr(0, head, pack->hlens - (head - pack->hdata), ":", 1);
        if (NULL == pos) {
            return ERR_FAILED;
        }
        field.key = head;
        field.klen = pos - head;
        if (0 == field.klen) {
            return ERR_FAILED;
        }
        head = pos + 1;
        head = skipempty(head, pack->hlens - (head - pack->hdata));
        if (NULL == head) {
            return ERR_FAILED;
        }
        pos = memstr(0, head, pack->hlens - (head - pack->hdata), FLAG_CRLF, flens);
        if (NULL == pos) {
            return ERR_FAILED;
        }
        field.value = head;
        field.vlen = pos - head;
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
    if (buffer_size(buf) >= pack->lens) {
        MALLOC(pack->data, pack->lens);
        ASSERTAB(pack->lens == buffer_remove(buf, pack->data, pack->lens), "copy buffer failed.");
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
    ((http_pack_ctx *)pack)->hdata = pack + sizeof(http_pack_ctx);
    ((http_pack_ctx *)pack)->hlens = lens;
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
    ASSERTAB(hlens == buffer_remove(buf, pack->hdata, hlens), "copy buffer failed.");
    if (ERR_OK != _http_parse_head(pack, status)) {
        *closefd = 1;
        LOG_NOEOFSTR(LOGLV_WARN, "http parse head failed.\n%s", pack->hdata, pack->hlens);
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
        if (PACK_TOO_LONG(pack->lens)) {
            *closefd = 1;
            http_pkfree(pack);
            LOG_WARN("http data too long, %"PRIu64, pack->lens);
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
        pctx->data = pack + sizeof(http_pack_ctx);
        pctx->lens = lens;
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
    drain = pack->lens + flens;
    if (buffer_size(buf) < drain) {
        return NULL;
    }
    if (pack->lens > 0) {
        ASSERTAB(pack->lens == buffer_copyout(buf, 0, pack->data, pack->lens), "copy buffer failed.");
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
    if (NULL != pack->hdata) {
        FREE(pack->data);
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
char *http_status(void *data, size_t *lens, int32_t index) {
    *lens = 0;
    char *status = NULL;
    http_pack_ctx *pack = data;
    switch (index) {
    case 1:
        status = pack->status1;
        *lens = pack->slens1;
        break;
    case 2:
        status = pack->status2;
        *lens = pack->slens2;
        break;
    case 3:
        status = pack->status3;
        *lens = pack->slens3;
        break;
    default:
        break;
    }
    return status;
}
size_t http_nheader(void *data) {
    return arr_header_size(&((http_pack_ctx *)data)->header);
}
http_header_ctx *http_header_at(void *data, size_t pos) {
    return arr_header_at(&((http_pack_ctx *)data)->header, pos);
}
char *http_header(void *data, const char *header, size_t *lens) {
    http_pack_ctx *pack = data;
    if (NULL == pack->hdata) {
        return NULL;
    }
    http_header_ctx *filed;
    for (size_t i = 0; i < arr_header_size(&pack->header); i++) {
        filed = arr_header_at(&pack->header, i);
        if (0 == _memicmp(filed->key, header, strlen(header))) {
            *lens = filed->vlen;
            return filed->value;
        }
    }
    return NULL;
}
int32_t http_chunked(void *data) {
    return ((http_pack_ctx *)data)->chunked;
}
void *http_data(void *data, size_t *lens) {
    *lens = ((http_pack_ctx *)data)->lens;
    return ((http_pack_ctx *)data)->data;
}
