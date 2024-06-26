#include "protocol/redis.h"
#include "protocol/protos.h"
#include "utils/binary.h"

typedef struct reader_ctx {
    redis_pack_ctx *head;
    redis_pack_ctx *tail;
    int64_t nelem;
}reader_ctx;

#define FMT_INTEGER_FLAG  "diouxX"
#define FMT_TYPE(type)\
    lens = (size_t)(f - p) + 1;\
    memcpy(_fmt, p, lens);\
    _fmt[lens] = '\0';\
    binary_set_va(&fbuf, _fmt, va_arg(args, type))

void _redis_pkfree(redis_pack_ctx *pack) {
    if (NULL == pack) {
        return;
    }
    redis_pack_ctx *next;
    do {
        next = pack->next;
        FREE(pack);
        pack = next;
    } while (NULL != pack);
}
void _redis_udfree(ud_cxt *ud) {
    if (NULL == ud->extra) {
        return;
    }
    reader_ctx *rd = ud->extra;
    _redis_pkfree(rd->head);
    FREE(rd);
    ud->extra = NULL;
}
static inline void _create_sds(binary_ctx *fbuf, binary_ctx *sdsbuf, size_t *n) {
    if (0 == fbuf->offset) {
        return;
    }
    binary_set_va(sdsbuf, "$%u"FLAG_CRLF, (uint32_t)fbuf->offset);
    binary_set_string(sdsbuf, fbuf->data, fbuf->offset);
    binary_set_string(sdsbuf, FLAG_CRLF, CRLF_SIZE);
    binary_offset(fbuf, 0);
    (*n)++;
}
static char *_redis_pack(size_t *size, const char *fmt, va_list args) {
    size_t lens, n = 0;
    binary_ctx fbuf, sdsbuf;
    binary_init(&fbuf, NULL, 0, 0);
    binary_init(&sdsbuf, NULL, 0, 0);
    char *p;
    char _fmt[64];
    char *f = (char *)fmt;
    while ('\0' != *f) {
        if ('%' != *f) {
            p = f;
            while ('\0' != *f && '%' != *f && ' ' != *f) f++;
            lens = (size_t)(f - p);
            if (lens > 0) {
                binary_set_string(&fbuf, p, lens);
            }
            if ('\0' == *f || ' ' == *f) {
                _create_sds(&fbuf, &sdsbuf, &n);
                if (' ' == *f) {
                    f++;
                }
            }
            continue;
        }
        p = f;
        f++;
        switch (*f) {
        case 's': {
            FMT_TYPE(char *);
            f++;
            break;
        }
        case 'b': {
            char *val = va_arg(args, char *);
            lens = va_arg(args, size_t);
            if (lens > 0) {
                binary_set_string(&fbuf, val, lens);
            }
            f++;
            break;
        }
        case 'c': {
            FMT_TYPE(int);
            f++;
            break;
        }
        case 'p': {
            FMT_TYPE(uintptr_t);
            f++;
            break;
        }
        case '%': {
            binary_set_string(&fbuf, "%", 1);
            f++;
            break;
        }
        default: {
            //跳过前缀
            while ('\0' != *f && NULL != strchr("#0-+ ", *f)) f++;
            while ('\0' != *f && isdigit((int)*f)) f++;
            if ('.' == *f) {
                f++;
                while ('\0' != *f && isdigit((int)*f)) f++;
            }
            if ('\0' == *f) {
                lens = (size_t)(f - p);
                if (lens >= 2) {
                    binary_set_string(&fbuf, p + 1, lens - 1);
                }
                break;
            }
            //double
            if (NULL != strchr("eEfFgGaA", *f)) {
                FMT_TYPE(double);
                f++;
                break;
            }
            //int
            if (NULL != strchr(FMT_INTEGER_FLAG, *f)) {
                FMT_TYPE(int);
                f++;
                break;
            }
            if ('h' == *f && 'h' == f[1]) {
                f += 2;
                if ('\0' != *f && NULL != strchr(FMT_INTEGER_FLAG, *f)) {
                    FMT_TYPE(int);
                    f++;
                } else {
                    binary_set_string(&fbuf, p + 1, (size_t)(f - p) - 1);
                }
                break;
            }
            if ('h' == *f) {
                f++;
                if ('\0' != *f && NULL != strchr(FMT_INTEGER_FLAG, *f)) {
                    FMT_TYPE(int);
                    f++;
                } else {
                    binary_set_string(&fbuf, p + 1, (size_t)(f - p) - 1);
                }
                break;
            }
            if ('l' == *f && 'l' == f[1]) {
                f += 2;
                if ('\0' != *f && NULL != strchr(FMT_INTEGER_FLAG, *f)) {
                    FMT_TYPE(long long);
                    f++;
                } else {
                    binary_set_string(&fbuf, p + 1, (size_t)(f - p) - 1);
                }
                break;
            }
            if ('l' == *f) {
                f++;
                if ('\0' != *f && NULL != strchr(FMT_INTEGER_FLAG, *f)) {
                    FMT_TYPE(long);
                    f++;
                } else {
                    binary_set_string(&fbuf, p + 1, (size_t)(f - p) - 1);
                }
                break;
            }
            lens = (size_t)(f - p);
            if (lens >= 2) {
                binary_set_string(&fbuf, p + 1, lens - 1);
            }
            break;
        }
        }
    }
    _create_sds(&fbuf, &sdsbuf, &n);
    SNPRINTF(_fmt, sizeof(_fmt), "*%d"FLAG_CRLF, (int32_t)n);
    size_t hlens = strlen(_fmt);
    *size = sdsbuf.offset + hlens;
    char *buf;
    MALLOC(buf, *size + 1);
    memcpy(buf, _fmt, hlens);
    memcpy(buf + hlens, sdsbuf.data, sdsbuf.offset);
    buf[*size] = '\0';
    FREE(fbuf.data);
    FREE(sdsbuf.data);
    return buf;
}
//%b:binary - size_t %%:%  C format
char *redis_pack(size_t *size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *buf = _redis_pack(size, fmt, args);
    va_end(args);
    return buf;
}
static reader_ctx *_create_reader(ud_cxt *ud) {
    if (NULL == ud->extra) {
        reader_ctx *rd;
        MALLOC(rd, sizeof(reader_ctx));
        rd->head = rd->tail = NULL;
        rd->nelem = 1;
        ud->extra = rd;
    }
    return ud->extra;
}
static inline void _add_node(reader_ctx *rd, redis_pack_ctx *pk) {
    if (NULL == rd->head) {
        rd->head = rd->tail = pk;
    } else {
        rd->tail->next = pk;
        rd->tail = pk;
    }
    if (RESP_ATTR != pk->proto) {
        rd->nelem--;
    }
}
//<type><data>\r\n
static int32_t _reader_line(reader_ctx *rd, int32_t proto, buffer_ctx *buf, int32_t *status) {
    int32_t pos = buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE);
    if (ERR_FAILED == pos) {
        BIT_SET(*status, PROTO_MOREDATA);
        return ERR_FAILED;
    }
    redis_pack_ctx *pk;
    CALLOC(pk, 1, sizeof(redis_pack_ctx) + pos);//前面还有1个type字节
    pk->proto = proto;
    pk->len = pos - 1;//pos 至少为1
    switch (proto) {
    case RESP_STRING:
    case RESP_ERROR:
        buffer_copyout(buf, 1, pk->data, (size_t)pk->len);
        break;
    case RESP_INTEGER:
    case RESP_BIGNUM:
        if (0 == pk->len) {
            BIT_SET(*status, PROTO_ERROR);
        } else {
            buffer_copyout(buf, 1, pk->data, (size_t)pk->len);
            char *end;
            pk->ival = strtoll(pk->data, &end, 10);
            if (end != pk->data + pk->len) {
                BIT_SET(*status, PROTO_ERROR);
            }
        }
        break;
    case RESP_NIL:
        if (0 != pk->len) {
            BIT_SET(*status, PROTO_ERROR);
        }
        break;
    case RESP_BOOL:
        if (1 != pk->len) {
            BIT_SET(*status, PROTO_ERROR);
            break;
        }
        buffer_copyout(buf, 1, pk->data, (size_t)pk->len);
        if (NULL == strchr("tTfF", pk->data[0])) {
            BIT_SET(*status, PROTO_ERROR);
            break;
        }
        if ('t' == pk->data[0] || 'T' == pk->data[0]) {
            pk->ival = 1;
        }
        break;
    case RESP_DOUBLE:
        if (0 == pk->len) {
            BIT_SET(*status, PROTO_ERROR);
            break;
        }
        buffer_copyout(buf, 1, pk->data, (size_t)pk->len);
        if (3 == pk->len && 0 == _memicmp(pk->data, "inf", (size_t)pk->len)) {
            pk->dval = INFINITY;
        } else if (4 == pk->len && 0 == _memicmp(pk->data, "-inf", (size_t)pk->len)) {
            pk->dval = -INFINITY;
        } else if ((3 == pk->len && 0 == _memicmp(pk->data, "nan", (size_t)pk->len))
                   ||(4 == pk->len && 0 == _memicmp(pk->data, "-nan", (size_t)pk->len))) {
            pk->dval = NAN;
        } else {
            char *end;
            pk->dval = strtod(pk->data, &end);
            if (end != pk->data + pk->len
                || !isfinite(pk->dval)) {
                BIT_SET(*status, PROTO_ERROR);
            }
        }
        break;
    default:
        break;
    }
    if (BIT_CHECK(*status, PROTO_ERROR)) {
        FREE(pk);
        return ERR_FAILED;
    }
    int32_t del = pos + CRLF_SIZE;
    ASSERTAB(del == buffer_drain(buf, del), "drain buffer failed.");
    _add_node(rd, pk);
    return ERR_OK;
}
//<type><length>\r\n<data>\r\n   <type>-1\r\n NUll
static int32_t _reader_bulk(reader_ctx *rd, int32_t proto, buffer_ctx *buf, int32_t *status) {
    int32_t pos = buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE);
    if (ERR_FAILED == pos) {
        BIT_SET(*status, PROTO_MOREDATA);
        return ERR_FAILED;
    }
    int32_t lens = pos - 1;
    if (0 == lens) {
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    char num[64];
    buffer_copyout(buf, 1, num, lens);
    num[lens] = '\0';
    char *end;
    int64_t blens = (int64_t)strtoll(num, &end, 10);
    if (num + lens != end
        || blens < -1) {
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    size_t total;
    if (-1 == blens) {
        redis_pack_ctx *pk;
        CALLOC(pk, 1, sizeof(redis_pack_ctx) + 1);
        pk->proto = proto;
        pk->len = blens;
        total = (size_t)(pos + CRLF_SIZE);
        ASSERTAB(total == buffer_drain(buf, total), "drain buffer failed.");
        _add_node(rd, pk);
        return ERR_OK;
    }
    total = (size_t)(blens + pos + CRLF_SIZE * 2);
    if (buffer_size(buf) < total) {
        BIT_SET(*status, PROTO_MOREDATA);
        return ERR_FAILED;
    }
    if ('\r' != buffer_at(buf, total - 2)
        || '\n' != buffer_at(buf, total - 1)) {
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    redis_pack_ctx *pk;
    CALLOC(pk, 1, sizeof(redis_pack_ctx) + (size_t)blens + 1);
    pk->proto = proto;
    switch (proto) {
    case RESP_BSTRING:
    case RESP_BERROR:
        pk->len = blens;
        buffer_copyout(buf, (size_t)(pos + CRLF_SIZE), pk->data, (size_t)pk->len);
        break;
    case RESP_VERB:
        if (blens < 4) {
            BIT_SET(*status, PROTO_ERROR);
            break;
        }
        if (':' != buffer_at(buf, (size_t)(pos + CRLF_SIZE + 3))) {
            BIT_SET(*status, PROTO_ERROR);
            break;
        }
        pk->len = blens - 4;
        buffer_copyout(buf, (size_t)(pos + CRLF_SIZE), pk->venc, 3);//3 bytes encoding
        buffer_copyout(buf, (size_t)(pos + CRLF_SIZE + 4), pk->data, (size_t)pk->len);
        break;
    default:
        break;
    }
    if (BIT_CHECK(*status, PROTO_ERROR)) {
        FREE(pk);
        return ERR_FAILED;
    }
    ASSERTAB(total == buffer_drain(buf, total), "drain buffer failed.");
    _add_node(rd, pk);
    return ERR_OK;
}
//<type><number-of-elements>\r\n<element-1>\r\n...<element-n>\r\n
static int32_t _reader_agg(reader_ctx *rd, int32_t proto, buffer_ctx *buf, int32_t *status) {
    int32_t pos = buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE);
    if (ERR_FAILED == pos) {
        BIT_SET(*status, PROTO_MOREDATA);
        return ERR_FAILED;
    }
    int32_t lens = pos - 1;
    if (0 == lens) {
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    char num[64];
    buffer_copyout(buf, 1, num, lens);
    num[lens] = '\0';
    char *end;
    int64_t nelem = (int64_t)strtoll(num, &end, 10);
    if (num + lens != end
        || nelem < -1) {
        BIT_SET(*status, PROTO_ERROR);
        return ERR_FAILED;
    }
    size_t del = (size_t)(pos + CRLF_SIZE);
    ASSERTAB(del == buffer_drain(buf, del), "drain buffer failed.");
    redis_pack_ctx *pk;
    CALLOC(pk, 1, sizeof(redis_pack_ctx) + 1);
    pk->proto = proto;
    pk->nelem = nelem;
    _add_node(rd, pk);
    if (nelem > 0) {
        rd->nelem += ((RESP_ATTR == proto || RESP_MAP == proto) ? nelem * 2 : nelem);
    }
    return ERR_OK;
}
redis_pack_ctx *redis_unpack(buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    int32_t rtn, proto;
    reader_ctx *rd = _create_reader(ud);
    for (;;) {
        if (buffer_size(buf) < (1 + CRLF_SIZE)) {
            BIT_SET(*status, PROTO_MOREDATA);
            break;
        }
        proto = buffer_at(buf, 0);
        switch (proto) {
        case RESP_STRING:
        case RESP_ERROR:
        case RESP_INTEGER:
        case RESP_NIL:
        case RESP_BOOL:
        case RESP_DOUBLE:
        case RESP_BIGNUM:
            rtn = _reader_line(rd, proto, buf, status);
            break;
        case RESP_BSTRING:
        case RESP_BERROR:
        case RESP_VERB:
            rtn = _reader_bulk(rd, proto, buf, status);
            break;
        case RESP_ARRAY:
        case RESP_SET:
        case RESP_PUSHE:
        case RESP_MAP:
        case RESP_ATTR:
            rtn = _reader_agg(rd, proto, buf, status);
            break;
        default:
            BIT_SET(*status, PROTO_ERROR);
            return NULL;
        }
        if (ERR_OK != rtn) {
            break;
        }
        if (0 == rd->nelem) {
            redis_pack_ctx *pk = rd->head;
            rd->head = rd->tail = NULL;
            rd->nelem = 1;
            return pk;
        }
    }
    return NULL;
}
