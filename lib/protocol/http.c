#include "protocol/http.h"
#include "protocol/prots.h"
#include "crypt/urlraw.h"
#include "containers/sarray.h"
#include "utils/utils.h"

typedef enum parse_status{
    INIT = 0,   // 初始状态，等待头部
    CONTENT,    // 已解析头部，等待 Content-Length 指定的数据体
    CHUNKED     // 分块传输模式
}parse_status;
ARRAY_DECL(http_header_ctx, arr_header);
typedef struct http_pack_ctx {
    int32_t chunked;          // 0=非 chunked，1=chunked 起始包，2=chunked 数据包
    buf_ctx head;             // 原始头部数据（含第一行和所有字段）
    buf_ctx data;             // 数据体
    buf_ctx status[3];        // 第一行拆分：status[0]=方法/版本，status[1]=状态码/路径，status[2]=描述/版本
    arr_header_ctx header;    // 所有头部字段列表
}http_pack_ctx;

#define MAX_HEADLENS ONEK * 4 // HTTP 头部最大允许长度（4 KB）
#define HEAD_REMAIN (pack->head.lens - (head - (char *)pack->head.data)) // 头部缓冲区剩余字节数

// 检查头部字段是否为 Content-Length 或 Transfer-Encoding: chunked，更新传输方式
static void _check_fileld(http_pack_ctx *pack, http_header_ctx *field, int32_t *transfer) {
    /* buf_icompare 内部先做长度比较（O(1)），各键长度均不同，无需首字符 switch */
    if (ERR_OK == _http_check_keyval(field,
                                     "content-length", sizeof("content-length") - 1,
                                     NULL, 0)) {
        *transfer = CONTENT;
        char _lenbuf[24];
        size_t _cplen = field->value.lens < sizeof(_lenbuf) - 1 ? field->value.lens : sizeof(_lenbuf) - 1;
        memcpy(_lenbuf, field->value.data, _cplen);
        _lenbuf[_cplen] = '\0';
        pack->data.lens = (size_t)strtoul(_lenbuf, NULL, 10);
    } else if (ERR_OK == _http_check_keyval(field,
                                            "transfer-encoding", sizeof("transfer-encoding") - 1,
                                            "chunked", sizeof("chunked") - 1)) {
        *transfer = CHUNKED;
        pack->data.lens = 0;
        pack->chunked = 1;
    }
}
// 解析 HTTP 第一行（请求行或状态行），填充 pack->status[0..2]，返回指向第一个头部字段的指针
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
// 解析全部头部字段，检测 Content-Length/Transfer-Encoding，将字段存入 pack->header
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
// 等待并读取 Content-Length 模式下的数据体，数据完整后重置 ud 状态
static http_pack_ctx *_http_content(buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    http_pack_ctx *pack = ud->context;
    if (buffer_size(buf) >= pack->data.lens) {
        MALLOC(pack->data.data, pack->data.lens);
        ASSERTAB(pack->data.lens == buffer_remove(buf, pack->data.data, pack->data.lens), "copy buffer failed.");
        ud->status = INIT;
        ud->context = NULL;
        return pack;
    } else {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
}
// 在缓冲区中搜索 \r\n\r\n，返回头部总长度（含结束 CRLF），未找到则设置状态标志
static size_t _http_headlens(buffer_ctx *buf, int32_t *status) {
    size_t flens = CRLF_SIZE * 2;
    int32_t pos = buffer_search(buf, 0, 0, 0, CONCAT2(FLAG_CRLF,FLAG_CRLF), flens);
    if (ERR_FAILED == pos) {
        if (buffer_size(buf) > MAX_HEADLENS) {
            BIT_SET(*status, PROT_ERROR);
        } else {
            BIT_SET(*status, PROT_MOREDATA);
        }
        return 0;
    }
    size_t hlens = pos + flens;
    if (hlens > MAX_HEADLENS) {
        BIT_SET(*status, PROT_ERROR);
        return 0;
    }
    return hlens;
}
// 分配 http_pack_ctx 结构体，头部数据紧随其后（连续内存），初始化头部字段数组
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
        BIT_SET(*status, PROT_ERROR);
        _http_pkfree(pack);
        return NULL;
    }
    return pack;
}
// 解析 HTTP 头部后根据传输方式决定：直接返回（无数据体/chunked）或进入数据体读取
static http_pack_ctx *_http_header(buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    int32_t transfer;
    http_pack_ctx *pack = _http_parsehead(buf, &transfer, status);
    if (NULL == pack) {
        return NULL;
    }
    if (CONTENT == transfer) {
        if (PACK_TOO_LONG(pack->data.lens)) {
            BIT_SET(*status, PROT_ERROR);
            _http_pkfree(pack);
            return NULL;
        } else {
            ud->context = pack;
            ud->status = transfer;
            return _http_content(buf, ud, status);
        }
    } else {
        if (1 == pack->chunked) {
            BIT_SET(*status, PROT_SLICE_START);
        }
        ud->status = transfer;
        return pack;
    }
}
// 分配 chunked 数据包结构体，lens>0 时数据紧随其后，chunked 字段固定设为 2
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
// 解析 chunked 编码的数据块：先读取长度行，再读取对应数据，长度为 0 表示结束
static http_pack_ctx *_http_chunked(buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t drain;
    http_pack_ctx *pack = ud->context;
    if (NULL == pack) {
        int32_t pos = buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE);
        if (ERR_FAILED == pos) {
            BIT_SET(*status, PROT_MOREDATA);
            return NULL;
        }
        char lensbuf[16] = { 0 };
        if (pos >= (int32_t)sizeof(lensbuf)) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        ASSERTAB(pos == (int32_t)buffer_copyout(buf, 0, lensbuf, pos), "copy buffer failed.");
        size_t dlens = strtol(lensbuf, NULL, 16);
        if (PACK_TOO_LONG(dlens)) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        drain = pos + CRLF_SIZE;
        ASSERTAB(drain == buffer_drain(buf, drain), "drain buffer failed.");
        pack = _http_chunkedpack(dlens);
        ud->context = pack;
    }
    drain = pack->data.lens + CRLF_SIZE;
    if (buffer_size(buf) < drain) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    if (pack->data.lens > 0) {
        BIT_SET(*status, PROT_SLICE);
        ASSERTAB(pack->data.lens == buffer_copyout(buf, 0, pack->data.data, pack->data.lens), "copy buffer failed.");
    } else {
        BIT_SET(*status, PROT_SLICE_END);
        ud->status = INIT;
    }
    ASSERTAB(drain == buffer_drain(buf, drain), "drain buffer failed.");
    ud->context = NULL;
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
    _http_pkfree(ud->context);
    ud->context = NULL;
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
        BIT_SET(*status, PROT_ERROR);
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
