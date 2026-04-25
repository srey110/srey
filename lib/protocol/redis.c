#include "protocol/redis.h"
#include "protocol/prots.h"
#include "utils/binary.h"
#include "containers/sarray.h"

/* 解析过程中积累的 redis_pack_ctx 指针数组。
 * 替代 reader_ctx 中的头尾链表：数组追加对缓存更友好，
 * 且在积累阶段避免了指针追踪开销。 */
ARRAY_DECL(redis_pack_ctx *, arr_rpk)

/* RESP 数组计数头 "*n\r\n" 所需的最大字节数。
 * 即使 "*999999\r\n" 也只有 10 字节；32 字节预留足够裕量。 */
#define MAX_HEADER_RESERVE  32

// 解包上下文，保存当前响应的所有节点和待解析元素计数
typedef struct reader_ctx {
    arr_rpk_ctx arr;  // 已解析节点的指针数组
    int64_t nelem;    // 还需解析的元素数（初始为 1，聚合类型会累加）
}reader_ctx;

#define FMT_INTEGER_FLAG  "diouxX" // 整型格式字符集
// 提取 [p, f] 范围的格式说明符，格式化并追加到 fbuf
#define FMT_TYPE(type)\
    lens = (size_t)(f - p) + 1;\
    memcpy(_fmt, p, lens);\
    _fmt[lens] = '\0';\
    binary_set_va(&fbuf, _fmt, va_arg(args, type))

void _redis_pkfree(redis_pack_ctx *pack) {
    if (NULL == pack) {
        return;
    }
    // 遍历链表逐节点释放
    redis_pack_ctx *next;
    do {
        next = pack->next;
        FREE(pack);
        pack = next;
    } while (NULL != pack);
}
void _redis_udfree(ud_cxt *ud) {
    if (NULL == ud->context) {
        return;
    }
    reader_ctx *rd = ud->context;
    /* 节点由扁平数组追踪；->next 仅在完整响应交给调用方时才串联。
     * 因此直接逐元素释放，无需走链表。 */
    for (uint32_t i = 0; i < arr_rpk_size(&rd->arr); i++) {
        FREE(*arr_rpk_at(&rd->arr, i));
    }
    arr_rpk_free(&rd->arr);
    FREE(rd);
    ud->context = NULL;
}
// 将 fbuf 中已积累的文本作为一个 RESP Bulk String 追加到 sdsbuf，并重置 fbuf 偏移，n 自增
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
// redis_pack 的内部实现，解析格式字符串并将各参数编码为 RESP Bulk String 序列
static char *_redis_pack(size_t *size, const char *fmt, va_list args) {
    size_t lens, n = 0;
    binary_ctx fbuf, sdsbuf;
    binary_init(&fbuf, NULL, 0, 0);
    binary_init(&sdsbuf, NULL, 0, 0);
    /* 在 sdsbuf 头部预留 MAX_HEADER_RESERVE 字节，用于回填 "*n\r\n" 头部；
     * 只有扫描完所有参数确定 n 之后才能写入该头部。 */
    binary_set_skip(&sdsbuf, MAX_HEADER_RESERVE);
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
            // 跳过格式标志位（#、0、-、+、空格）
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
    /* 格式化 RESP 数组计数头并回填到 sdsbuf 头部预留槽中。
     * memmove 将 Bulk String 主体向左移动以消除填充间隙，
     * 避免额外的输出内存分配。 */
    int hlen_int = SNPRINTF(_fmt, sizeof(_fmt), "*%d"FLAG_CRLF, (int32_t)n);
    size_t hlens     = (size_t)hlen_int;
    size_t body_size = sdsbuf.offset - MAX_HEADER_RESERVE;
    ASSERTAB(hlens <= MAX_HEADER_RESERVE, "RESP header too long for reserved slot.");
    memmove(sdsbuf.data + hlens, sdsbuf.data + MAX_HEADER_RESERVE, body_size);
    memcpy(sdsbuf.data, _fmt, hlens);
    *size = hlens + body_size;
    /* *size < MAX_HEADER_RESERVE + body_size == sdsbuf.offset <= sdsbuf.size */
    sdsbuf.data[*size] = '\0';
    FREE(fbuf.data);
    return sdsbuf.data; // 调用方负责释放此内存
}
// redis_pack 的公开入口，负责初始化 va_list 后委托给 _redis_pack
char *redis_pack(size_t *size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *buf = _redis_pack(size, fmt, args);
    va_end(args);
    return buf;
}
// 获取或创建 ud_cxt 关联的解包上下文（首次调用时分配并初始化）
static reader_ctx *_create_reader(ud_cxt *ud) {
    if (NULL == ud->context) {
        reader_ctx *rd;
        MALLOC(rd, sizeof(reader_ctx));
        arr_rpk_init(&rd->arr, 0);
        rd->nelem = 1; // 初始期望解析 1 个顶层元素
        ud->context = rd;
    }
    return ud->context;
}
// 将已解析节点追加到数组，并递减待解析计数（属性类型 RESP_ATTR 不计入计数）
static inline void _add_node(reader_ctx *rd, redis_pack_ctx *pk) {
    arr_rpk_push_back(&rd->arr, &pk);
    if (RESP_ATTR != pk->prot) {
        rd->nelem--;
    }
}
// 解析单行类型（简单字符串/错误/整数/空值/布尔/浮点/大整数）：格式 <type><data>\r\n
static int32_t _reader_line(reader_ctx *rd, int32_t prot, buffer_ctx *buf, int32_t *status) {
    int32_t pos = buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE);
    if (ERR_FAILED == pos) {
        BIT_SET(*status, PROT_MOREDATA);
        return ERR_FAILED;
    }
    redis_pack_ctx *pk;
    CALLOC(pk, 1, sizeof(redis_pack_ctx) + pos);//前面还有1个type字节
    pk->prot = prot;
    pk->len = pos - 1;//pos 起始为1
    switch (prot) {
    case RESP_STRING:
    case RESP_ERROR:
        buffer_copyout(buf, 1, pk->data, (size_t)pk->len);
        break;
    case RESP_INTEGER:
    case RESP_BIGNUM:
        if (0 == pk->len) {
            BIT_SET(*status, PROT_ERROR);
        } else {
            buffer_copyout(buf, 1, pk->data, (size_t)pk->len);
            char *end;
            pk->ival = strtoll(pk->data, &end, 10);
            if (end != pk->data + pk->len) {
                BIT_SET(*status, PROT_ERROR);
            }
        }
        break;
    case RESP_NIL:
        if (0 != pk->len) {
            BIT_SET(*status, PROT_ERROR);
        }
        break;
    case RESP_BOOL:
        if (1 != pk->len) {
            BIT_SET(*status, PROT_ERROR);
            break;
        }
        buffer_copyout(buf, 1, pk->data, (size_t)pk->len);
        if (NULL == strchr("tTfF", pk->data[0])) {
            BIT_SET(*status, PROT_ERROR);
            break;
        }
        if ('t' == pk->data[0] || 'T' == pk->data[0]) {
            pk->ival = 1;
        }
        break;
    case RESP_DOUBLE:
        if (0 == pk->len) {
            BIT_SET(*status, PROT_ERROR);
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
                BIT_SET(*status, PROT_ERROR);
            }
        }
        break;
    default:
        break;
    }
    if (BIT_CHECK(*status, PROT_ERROR)) {
        FREE(pk);
        return ERR_FAILED;
    }
    int32_t del = pos + CRLF_SIZE;
    ASSERTAB(del == (int32_t)buffer_drain(buf, del), "drain buffer failed.");
    _add_node(rd, pk);
    return ERR_OK;
}
// 解析批量字符串类型（Bulk String/Error/Verbatim）：格式 <type><length>\r\n<data>\r\n，长度为 -1 表示 Null
static int32_t _reader_bulk(reader_ctx *rd, int32_t prot, buffer_ctx *buf, int32_t *status) {
    int32_t pos = buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE);
    if (ERR_FAILED == pos) {
        BIT_SET(*status, PROT_MOREDATA);
        return ERR_FAILED;
    }
    int32_t lens = pos - 1;
    if (0 == lens) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    char num[64];
    buffer_copyout(buf, 1, num, lens);
    num[lens] = '\0';
    char *end;
    int64_t blens = (int64_t)strtoll(num, &end, 10);
    if (num + lens != end
        || blens < -1) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    size_t total;
    if (-1 == blens) {
        redis_pack_ctx *pk;
        CALLOC(pk, 1, sizeof(redis_pack_ctx) + 1);
        pk->prot = prot;
        pk->len = blens;
        total = (size_t)(pos + CRLF_SIZE);
        ASSERTAB(total == buffer_drain(buf, total), "drain buffer failed.");
        _add_node(rd, pk);
        return ERR_OK;
    }
    total = (size_t)(blens + pos + CRLF_SIZE * 2);
    if (buffer_size(buf) < total) {
        BIT_SET(*status, PROT_MOREDATA);
        return ERR_FAILED;
    }
    if ('\r' != buffer_at(buf, total - 2)
        || '\n' != buffer_at(buf, total - 1)) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    redis_pack_ctx *pk;
    CALLOC(pk, 1, sizeof(redis_pack_ctx) + (size_t)blens + 1);
    pk->prot = prot;
    switch (prot) {
    case RESP_BSTRING:
    case RESP_BERROR:
        pk->len = blens;
        buffer_copyout(buf, (size_t)(pos + CRLF_SIZE), pk->data, (size_t)pk->len);
        break;
    case RESP_VERB:
        if (blens < 4) {
            BIT_SET(*status, PROT_ERROR);
            break;
        }
        if (':' != buffer_at(buf, (size_t)(pos + CRLF_SIZE + 3))) {
            BIT_SET(*status, PROT_ERROR);
            break;
        }
        pk->len = blens - 4;
        buffer_copyout(buf, (size_t)(pos + CRLF_SIZE), pk->venc, 3);//3 bytes encoding
        buffer_copyout(buf, (size_t)(pos + CRLF_SIZE + 4), pk->data, (size_t)pk->len);
        break;
    default:
        break;
    }
    if (BIT_CHECK(*status, PROT_ERROR)) {
        FREE(pk);
        return ERR_FAILED;
    }
    ASSERTAB(total == buffer_drain(buf, total), "drain buffer failed.");
    _add_node(rd, pk);
    return ERR_OK;
}
// 解析聚合类型（数组/集合/推送/映射/属性）：格式 <type><number-of-elements>\r\n<element-1>...<element-n>
static int32_t _reader_agg(reader_ctx *rd, int32_t prot, buffer_ctx *buf, int32_t *status) {
    int32_t pos = buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE);
    if (ERR_FAILED == pos) {
        BIT_SET(*status, PROT_MOREDATA);
        return ERR_FAILED;
    }
    int32_t lens = pos - 1;
    if (0 == lens) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    char num[64];
    buffer_copyout(buf, 1, num, lens);
    num[lens] = '\0';
    char *end;
    int64_t nelem = (int64_t)strtoll(num, &end, 10);
    if (num + lens != end
        || nelem < -1) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    size_t del = (size_t)(pos + CRLF_SIZE);
    ASSERTAB(del == buffer_drain(buf, del), "drain buffer failed.");
    redis_pack_ctx *pk;
    CALLOC(pk, 1, sizeof(redis_pack_ctx) + 1);
    pk->prot = prot;
    pk->nelem = nelem;
    _add_node(rd, pk);
    if (nelem > 0) {
        rd->nelem += ((RESP_ATTR == prot || RESP_MAP == prot) ? nelem * 2 : nelem);
    }
    return ERR_OK;
}
redis_pack_ctx *redis_unpack(buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    int32_t rtn, prot;
    reader_ctx *rd = _create_reader(ud);
    for (;;) {
        if (buffer_size(buf) < (1 + CRLF_SIZE)) {
            BIT_SET(*status, PROT_MOREDATA);
            break;
        }
        prot = buffer_at(buf, 0);
        switch (prot) {
        case RESP_STRING:
        case RESP_ERROR:
        case RESP_INTEGER:
        case RESP_NIL:
        case RESP_BOOL:
        case RESP_DOUBLE:
        case RESP_BIGNUM:
            rtn = _reader_line(rd, prot, buf, status);
            break;
        case RESP_BSTRING:
        case RESP_BERROR:
        case RESP_VERB:
            rtn = _reader_bulk(rd, prot, buf, status);
            break;
        case RESP_ARRAY:
        case RESP_SET:
        case RESP_PUSHE:
        case RESP_MAP:
        case RESP_ATTR:
            rtn = _reader_agg(rd, prot, buf, status);
            break;
        default:
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        if (ERR_OK != rtn) {
            break;
        }
        if (0 == rd->nelem) {
            uint32_t cnt = arr_rpk_size(&rd->arr);
            for (uint32_t i = 0; i + 1 < cnt; i++) {
                (*arr_rpk_at(&rd->arr, i))->next = *arr_rpk_at(&rd->arr, i + 1);
            }
            if (cnt > 0) {
                (*arr_rpk_at(&rd->arr, cnt - 1))->next = NULL;
            }
            redis_pack_ctx *pk = (cnt > 0) ? *arr_rpk_at(&rd->arr, 0) : NULL;
            arr_rpk_clear(&rd->arr);
            rd->nelem = 1;
            return pk;
        }
    }
    return NULL;
}
