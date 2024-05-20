#include "proto/redis.h"
#include "ds/sarray.h"

typedef struct mark_ctx {
    resp_type type;
    size_t element;//总节点数
    size_t count;//当前读取到的数量
}mark_ctx;
ARRAY_DECL(mark_ctx, arr_mark);
typedef struct reader_ctx {
    redis_pack_ctx *head;
    redis_pack_ctx *tail;
    arr_mark_ctx mark;
}reader_ctx;

#define MAX_BULK_SIZE ONEK * ONEK * 512
#define FMT_INTEGER_FLAG  "diouxX"
#define FMT_TYPE(type)\
    lens = (size_t)(f - p) + 1;\
    memcpy(_fmt, p, lens);\
    _fmt[lens] = '\0';\
    buffer_appendv(&fbuf, _fmt, va_arg(args, type))

void redis_pkfree(redis_pack_ctx *pack) {
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
void redis_udfree(ud_cxt *ud) {
    if (NULL == ud->extra) {
        return;
    }
    reader_ctx *rd = ud->extra;
    redis_pkfree(rd->head);
    arr_mark_free(&rd->mark);
    FREE(rd);
    ud->extra = NULL;
}
static inline void _create_sds(buffer_ctx *fbuf, buffer_ctx *sdsbuf, size_t *n) {
    size_t lens = buffer_size(fbuf);
    if (0 == lens) {
        return;
    }
    char *buf;
    MALLOC(buf, lens);
    buffer_remove(fbuf, buf, lens);
    buffer_appendv(sdsbuf, "$%u"FLAG_CRLF, (uint32_t)lens);
    buffer_append(sdsbuf, buf, lens);
    buffer_append(sdsbuf, FLAG_CRLF, CRLF_SIZE);
    FREE(buf);
    (*n)++;
}
static char *_redis_pack(size_t *size, const char *fmt, va_list args) {
    size_t lens, n = 0;
    buffer_ctx fbuf, sdsbuf;
    buffer_init(&fbuf);
    buffer_init(&sdsbuf);
    char *p;
    char _fmt[64];
    char *f = (char *)fmt;
    while ('\0' != *f) {
        if ('%' != *f) {
            //跳过多余空格
            while (' ' == *f && ' ' == f[1]) f++;
            p = f;
            while ('\0' != *f && '%' != *f && ' ' != *f) f++;
            lens = (size_t)(f - p);
            if (lens > 0) {
                buffer_append(&fbuf, p, lens);
            }
            if ('\0' == *f || ' ' == *f) {
                _create_sds(&fbuf, &sdsbuf, &n);
                //跳过空格
                while (' ' == *f) f++;
            }
            continue;
        }
        //%
        p = f;
        f++;
        if ('\0' == *f) {
            buffer_append(&fbuf, "%", 1);
            break;
        }
        //跳过前缀
        while ('\0' != *f && NULL != strchr("#0-+ ", *f)) f++;
        while ('\0' != *f && isdigit((int)*f)) f++;
        if ('.' == *f) {
            f++;
            while ('\0' != *f && isdigit((int)*f)) f++;
        }
        if ('\0' == *f) {
            buffer_append(&fbuf, p, (size_t)(f - p));
            break;
        }
        if ((size_t)(f - p) + 3 >= sizeof(_fmt)) {
            buffer_append(&fbuf, p, (size_t)(f - p));
            continue;
        }
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
                buffer_append(&fbuf, val, lens);
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
            buffer_append(&fbuf, "%", 1);
            f++;
            break;
        }
        default: {
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
            //char
            if ('h' == *f && 'h' == f[1]) {
                f += 2;
                if ('\0' != *f && NULL != strchr(FMT_INTEGER_FLAG, *f)) {
                    FMT_TYPE(int);
                    f++;
                } else {
                    buffer_append(&fbuf, p, (size_t)(f - p));
                }
                break;
            }
            //short
            if ('h' == *f) {
                f++;
                if ('\0' != *f && NULL != strchr(FMT_INTEGER_FLAG, *f)) {
                    FMT_TYPE(int);
                    f++;
                } else {
                    buffer_append(&fbuf, p, (size_t)(f - p));
                }
                break;
            }
            //long long
            if ('l' == *f && 'l' == f[1]) {
                f += 2;
                if ('\0' != *f && NULL != strchr(FMT_INTEGER_FLAG, *f)) {
                    FMT_TYPE(long long);
                    f++;
                } else {
                    buffer_append(&fbuf, p, (size_t)(f - p));
                }
                break;
            }
            //long
            if ('l' == *f) {
                f++;
                if ('\0' != *f && NULL != strchr(FMT_INTEGER_FLAG, *f)) {
                    FMT_TYPE(long);
                    f++;
                } else {
                    buffer_append(&fbuf, p, (size_t)(f - p));
                }
                break;
            }
            buffer_append(&fbuf, p, (size_t)(f - p));
            break;
        }
        }
    }
    _create_sds(&fbuf, &sdsbuf, &n);
    SNPRINTF(_fmt, sizeof(_fmt) - 1, "*%d"FLAG_CRLF, (int32_t)n);
    size_t hlens = strlen(_fmt);
    lens = buffer_size(&sdsbuf);
    *size = lens + hlens;
    char *buf;
    MALLOC(buf, *size + 1);
    memcpy(buf, _fmt, hlens);
    buffer_copyout(&sdsbuf, 0, buf + hlens, lens);
    buf[*size] = '\0';
    buffer_free(&fbuf);
    buffer_free(&sdsbuf);
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
static inline resp_type _resp_type(char type) {
    switch (type) {
    case '+':
        return RESP_STRING;
    case '-':
        return RESP_ERROR;
    case ':':
        return RESP_INTEGER;
    case '_':
        return RESP_NULL;
    case '#':
        return RESP_BOOL;
    case ',':
        return RESP_DOUBLE;
    case '(':
        return RESP_BIG_NUMBER;
    case '$':
        return RESP_BULK_STRING;
    case '!':
        return RESP_BULK_ERROR;
    case '=':
        return RESP_VERB_STRING;
    case '*':
        return RESP_ARRAY;
    case '%':
        return RESP_MAP;
    case '~':
        return RESP_SET;
    case '>':
        return RESP_PUSHE;
    default:
        return RESP_NONE;
    }
}
static reader_ctx *_create_reader(ud_cxt *ud) {
    if (NULL == ud->extra) {
        reader_ctx *rd;
        MALLOC(rd, sizeof(reader_ctx));
        rd->head = rd->tail = NULL;
        arr_mark_init(&rd->mark, 0);
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
}
static int32_t _reader_simple(reader_ctx *rd, resp_type rtype, buffer_ctx *buf, int32_t *closefd) {
    int32_t pos = buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE);
    if (ERR_FAILED == pos) {
        return ERR_FAILED;
    }
    redis_pack_ctx *pk;
    CALLOC(pk, 1, sizeof(redis_pack_ctx) + pos);//前面还有1个type字节
    pk->type = rtype;
    pk->len = (size_t)(pos - 1);
    switch (rtype) {
    case RESP_STRING:
    case RESP_ERROR:
        if (0 == pk->len) {
            *closefd = 1;
        } else {
            buffer_copyout(buf, 1, pk->data, pk->len);
        }
        break;
    case RESP_INTEGER:
    case RESP_BIG_NUMBER:
        if (0 == pk->len) {
            *closefd = 1;
        } else {
            buffer_copyout(buf, 1, pk->data, pk->len);
            char *end;
            pk->ival = strtoll(pk->data, &end, 10);
            if (end != pk->data + pk->len) {
                *closefd = 1;
            }
        }
        break;
    case RESP_NULL:
        if (0 != pk->len) {
            *closefd = 1;
        }
        break;
    case RESP_BOOL:
        if (1 != pk->len) {
            *closefd = 1;
            break;
        }
        buffer_copyout(buf, 1, pk->data, pk->len);
        if (NULL == strchr("tTfF", pk->data[0])) {
            *closefd = 1;
            break;
        }
        if ('t' == pk->data[0] || 'T' == pk->data[0]) {
            pk->ival = 1;
        }
        break;
    case RESP_DOUBLE:
        if (0 == pk->len) {
            *closefd = 1;
            break;
        }
        buffer_copyout(buf, 1, pk->data, pk->len);
        if (3 == pk->len && 0 == _memicmp(pk->data, "inf", pk->len)) {
            pk->dval = INFINITY;
        } else if (4 == pk->len && 0 == _memicmp(pk->data, "-inf", pk->len)) {
            pk->dval = -INFINITY;
        } else if ((3 == pk->len && 0 == _memicmp(pk->data, "nan", pk->len))
                    ||(4 == pk->len && 0 == _memicmp(pk->data, "-nan", pk->len))) {
            pk->dval = NAN;
        } else {
            char *end;
            pk->dval = strtod(pk->data, &end);
            if (!isfinite(pk->dval) 
                || end != pk->data + pk->len) {
                *closefd = 1;
            }
        }
        break;
    default:
        break;
    }
    if (0 != *closefd) {
        FREE(pk);
        return ERR_FAILED;
    }
    int32_t del = pos + CRLF_SIZE;
    ASSERTAB(del == buffer_drain(buf, del), "drain buffer failed.");
    _add_node(rd, pk);
    return ERR_OK;
}
static int32_t _reader_bulk(reader_ctx *rd, resp_type rtype, buffer_ctx *buf, int32_t *closefd) {
    int32_t pos = buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE);
    if (ERR_FAILED == pos) {
        return ERR_FAILED;
    }
    int32_t lens = pos - 1;
    if (0 == lens) {
        *closefd = 1;
        return ERR_FAILED;
    }
    char num[64];
    buffer_copyout(buf, 1, num, lens);
    num[lens] = '\0';
    char *end;
    int32_t blens = (int32_t)strtol(num, &end, 10);
    if (num + lens != end
        || blens > MAX_BULK_SIZE
        || blens < 0) {
        *closefd = 1;
        return ERR_FAILED;
    }
    size_t total = (size_t)(blens + pos + CRLF_SIZE * 2);
    if (buffer_size(buf) < total) {
        return ERR_FAILED;
    }
    if ('\r' != buffer_at(buf, total - 2)
        || '\n' != buffer_at(buf, total - 1)) {
        *closefd = 1;
        return ERR_FAILED;
    }
    redis_pack_ctx *pk;
    CALLOC(pk, 1, sizeof(redis_pack_ctx) + (size_t)blens + 1);
    pk->type = rtype;
    switch (rtype) {
    case RESP_BULK_STRING:
    case RESP_BULK_ERROR: 
        pk->len = (size_t)blens;
        if (pk->len > 0) {
            buffer_copyout(buf, (size_t)(pos + CRLF_SIZE), pk->data, pk->len);
        }
        break;
    case RESP_VERB_STRING:
        if (blens < 4) {
            *closefd = 1;
            break;
        }
        if (':' != buffer_at(buf, pos + CRLF_SIZE + 3)) {
            *closefd = 1;
            break;
        }
        pk->len = blens - 4;
        buffer_copyout(buf, pos + CRLF_SIZE, pk->vtype, 3);//3 bytes encoding
        if (pk->len > 0) {
            buffer_copyout(buf, pos + CRLF_SIZE + 4, pk->data, pk->len);
        }
        break;
    default:
        break;
    }
    if (0 != *closefd) {
        FREE(pk);
        return ERR_FAILED;
    }
    ASSERTAB(total == buffer_drain(buf, total), "drain buffer failed.");
    _add_node(rd, pk);
    return ERR_OK;
}
static int32_t _reader_aggregate(reader_ctx *rd, resp_type rtype, buffer_ctx *buf, int32_t *closefd) {
    int32_t pos = buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE);
    if (ERR_FAILED == pos) {
        return ERR_FAILED;
    }
    int32_t lens = pos - 1;
    if (0 == lens) {
        *closefd = 1;
        return ERR_FAILED;
    }
    char num[64];
    buffer_copyout(buf, 1, num, lens);
    num[lens] = '\0';
    char *end;
    int32_t element = (int32_t)strtol(num, &end, 10);
    if (num + lens != end
        || element < 0) {
        *closefd = 1;
        return ERR_FAILED;
    }
    redis_pack_ctx *pk;
    CALLOC(pk, 1, sizeof(redis_pack_ctx) + 1);
    pk->type = rtype;
    pk->element = (size_t)element;
    if (pk->element > 0) {
        mark_ctx mk;
        mk.count = 0;
        mk.element = pk->element;
        mk.type = pk->type;
        arr_mark_push_back(&rd->mark, &mk);
    }
    int32_t del = pos + CRLF_SIZE;
    ASSERTAB(del == buffer_drain(buf, del), "drain buffer failed.");
    _add_node(rd, pk);
    return ERR_OK;
}
static inline void _update_mark(reader_ctx *rd) {
    int32_t size = (int32_t)arr_mark_size(&rd->mark);
    if (0 == size) {
        return;
    }
    //向前加
    mark_ctx *mk;
    for (int32_t i = size - 1; i >= 0; i--) {
        mk = arr_mark_at(&rd->mark, i);
        mk->count++;
        if (RESP_MAP == mk->type) {
            if (mk->count >= mk->element * 2) {
                arr_mark_pop_back(&rd->mark);
            } else {
                break;
            }
        } else {
            if (mk->count >= mk->element) {
                arr_mark_pop_back(&rd->mark);
            } else {
                break;
            }
        }
    }
}
redis_pack_ctx *redis_unpack(buffer_ctx *buf, ud_cxt *ud, int32_t *closefd) {
    resp_type rtype;
    int32_t rtn = ERR_OK;
    reader_ctx *rd = _create_reader(ud);
    for (;;) {
        if (buffer_size(buf) < 1 + CRLF_SIZE) {
            break;
        }
        rtype = _resp_type(buffer_at(buf, 0));
        switch (rtype) {
        case RESP_STRING:
        case RESP_ERROR:
        case RESP_INTEGER:
        case RESP_NULL:
        case RESP_BOOL:
        case RESP_DOUBLE:
        case RESP_BIG_NUMBER:
            rtn = _reader_simple(rd, rtype, buf, closefd);
            if (ERR_OK == rtn) {
                _update_mark(rd);
            }
            break;
        case RESP_BULK_STRING:
        case RESP_BULK_ERROR:
        case RESP_VERB_STRING:
            rtn = _reader_bulk(rd, rtype, buf, closefd);
            if (ERR_OK == rtn) {
                _update_mark(rd);
            }
            break;
        case RESP_ARRAY:
        case RESP_MAP:
        case RESP_SET:
        case RESP_PUSHE:
            rtn = _reader_aggregate(rd, rtype, buf, closefd);
            if (ERR_OK == rtn
                && 0 == rd->tail->element) {
                _update_mark(rd);
            }
            break;
        default:
            *closefd = 1;
            return NULL;
        }
        if (ERR_OK != rtn
            || 0 != *closefd) {
            break;
        }
        if (0 == arr_mark_size(&rd->mark)) {
            redis_pack_ctx *pk = rd->head;
            rd->head = rd->tail = NULL;
            return pk;
        }
    }
    return NULL;
}
