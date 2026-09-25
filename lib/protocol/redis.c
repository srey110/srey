#include "protocol/redis.h"
#include "utils/binary.h"

/* RESP 数组计数头 "*n\r\n" 所需的最大字节数。
 * 即使 "*999999\r\n" 也只有 10 字节；32 字节预留足够裕量。 */
#define MAX_HEADER_RESERVE  32
//最大聚合嵌套层数，防御恶意 server 用 *1\r\n*1\r\n... 嵌套数组导致堆 OOM
//（每层嵌套都只是再压一帧、永不清空帧栈，故永不触发完整 pack 返回，节点持续累积到 rd 的链表上）
//含 stack[0] 顶层虚拟帧，故实际可嵌套 REDIS_MAX_DEPTH-1 层
#define REDIS_MAX_DEPTH     18
//单次 RESP 解包最大节点数，纯内存兜底；正确性由 REDIS_MAX_DEPTH 保证，
//此值只决定为一个未完成回复最多垫多少内存，不应低到误伤合法的扁平大结果集。
//1<<18 时最坏约 17MB（节点约 64B/个），仍是现有 7 万元素用例的 3.7 倍余量；
//再往上抬只是让恶意 server 能用约 4MB 流量顶出几十 MB，业务侧的大结果集应分页取
#define REDIS_MAX_NODES     (1 << 18)
#define FMT_INTEGER_FLAG  "diouxX" // 整型格式字符集
// 提取 [p, f] 范围的格式说明符，格式化并追加到 fbuf
#define FMT_TYPE(type)\
    lens = (size_t)(f - p) + 1;\
    ASSERTAB(lens < sizeof(_fmt), "redis format specifier too long");\
    memcpy(_fmt, p, lens);\
    _fmt[lens] = '\0';\
    binary_set_va(&fbuf, _fmt, va_arg(args, type))

// 解包上下文，保存当前响应的所有节点与未闭合聚合层的帧栈
// 聚合层帧：一层未闭合的聚合类型
typedef struct redis_frame {
    int32_t attr;     // 1 = RESP_ATTR 层：RESP 规定属性不计入父层元素数，故本层闭合时不消费父层一格
    int64_t remain;   // 本层还差多少元素才闭合
}redis_frame;
typedef struct reader_ctx {
    int32_t depth;          // 当前未闭合聚合层数，stack[0] 为顶层虚拟帧；归零即一条完整回复解析完毕
    uint32_t count;         // 当前回复已解析的节点数，REDIS_MAX_NODES 兜底用
    redis_pack_ctx *head;   // 已解析节点按到达顺序串成的链表，回复完整后整条交给调用方
    redis_pack_ctx *tail;   // 链表尾，尾插用
    redis_frame stack[REDIS_MAX_DEPTH];
}reader_ctx;

void _redis_pkfree(void *data) {
    redis_pack_ctx *pack = (redis_pack_ctx *)data;
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
    _redis_pkfree(rd->head);// 未完成回复里已解析的节点
    FREE(rd);
    ud->context = NULL;
}
// pending 非 0（当前位置存在参数，含空串）时将 fbuf 内容作为一个 RESP Bulk String 追加到 sdsbuf（空参数输出 $0）；重置 fbuf 偏移与 pending，n 自增
static inline void _redis_create_sds(binary_ctx *fbuf, binary_ctx *sdsbuf, size_t *n, int32_t *pending) {
    if (0 == *pending) {
        return;
    }
    binary_set_va(sdsbuf, "$%zu"FLAG_CRLF, fbuf->offset);
    binary_set_binary(sdsbuf, fbuf->data, fbuf->offset);
    binary_set_binary(sdsbuf, FLAG_CRLF, CRLF_SIZE);
    binary_offset(fbuf, 0);
    *pending = 0;
    (*n)++;
}
// redis_pack 的内部实现，解析格式字符串并将各参数编码为 RESP Bulk String 序列
static char *_redis_pack(size_t *size, const char *fmt, va_list args) {
    size_t lens, n = 0;
    int32_t pending = 0;
    int32_t fmterr = 0;
    binary_ctx fbuf, sdsbuf;
    binary_init_write(&fbuf, 0, 0);
    binary_init_write(&sdsbuf, 0, 0);
    /* 在 sdsbuf 头部预留 MAX_HEADER_RESERVE 字节，用于回填 "*n\r\n" 头部；
     * 只有扫描完所有参数确定 n 之后才能写入该头部。 */
    binary_set_skip(&sdsbuf, MAX_HEADER_RESERVE);
    char *p;
    char *val;
    char _fmt[64];
    char *f = (char *)fmt;
    while ('\0' != *f) {
        if ('%' != *f) {
            p = f;
            while ('\0' != *f && '%' != *f && ' ' != *f) {
                f++;
            }
            lens = (size_t)(f - p);
            if (lens > 0) {
                binary_set_binary(&fbuf, p, lens);
                pending = 1;
            }
            if ('\0' == *f || ' ' == *f) {
                _redis_create_sds(&fbuf, &sdsbuf, &n, &pending);
                if (' ' == *f) {
                    f++;
                }
            }
            continue;
        }
        p = f;
        f++;
        pending = 1;
        switch (*f) {
        case 's': {
            FMT_TYPE(char *);
            f++;
            break;
        }
        case 'b': {
            val = va_arg(args, char *);
            lens = va_arg(args, size_t);
            if (lens > 0) {
                binary_set_binary(&fbuf, val, lens);
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
            // 必须取 void *：调用方按 %p 压进来的就是它，取成 uintptr_t 两次都不合类型
            // （va_arg 一次，转手喂给 vsnprintf 的 %p 又一次）
            FMT_TYPE(void *);
            f++;
            break;
        }
        case '%': {
            binary_set_binary(&fbuf, "%", 1);
            f++;
            break;
        }
        default: {
            // 跳过格式标志位（#、0、-、+、空格）
            while ('\0' != *f && NULL != strchr("#0-+ ", *f)) {
                f++;
            }
            while ('\0' != *f && isdigit((unsigned char)*f)) {
                f++;
            }
            if ('.' == *f) {
                f++;
                while ('\0' != *f && isdigit((unsigned char)*f)) {
                    f++;
                }
            }
            if ('\0' == *f) {// '%' 之后只有标志/宽度就到串尾，没有转换符
                fmterr = 1;
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
                } else {// 长度修饰后面不是整数转换
                    fmterr = 1;
                }
                break;
            }
            if ('h' == *f) {
                f++;
                if ('\0' != *f && NULL != strchr(FMT_INTEGER_FLAG, *f)) {
                    FMT_TYPE(int);
                    f++;
                } else {// 长度修饰后面不是整数转换
                    fmterr = 1;
                }
                break;
            }
            if ('l' == *f && 'l' == f[1]) {
                f += 2;
                if ('\0' != *f && NULL != strchr(FMT_INTEGER_FLAG, *f)) {
                    FMT_TYPE(long long);
                    f++;
                } else {// 长度修饰后面不是整数转换
                    fmterr = 1;
                }
                break;
            }
            if ('l' == *f) {
                f++;
                if ('\0' != *f && NULL != strchr(FMT_INTEGER_FLAG, *f)) {
                    FMT_TYPE(long);
                    f++;
                } else {// 长度修饰后面不是整数转换
                    fmterr = 1;
                }
                break;
            }
            // size_t %zu 是调用方最自然的写法，单独认一下；不认的话它会掉进末尾那条整体拒绝
            if ('z' == *f) {
                f++;
                if ('\0' != *f && NULL != strchr(FMT_INTEGER_FLAG, *f)) {
                    FMT_TYPE(size_t);
                    f++;
                } else {// 长度修饰后面不是整数转换
                    fmterr = 1;
                }
                break;
            }
            fmterr = 1;
            break;
        }
        }
        if (0 != fmterr) {
            LOG_ERROR("redis_pack: unsupported conversion at offset %zu of format \"%s\".",
                      (size_t)(p - fmt), fmt);
            binary_free(&fbuf);
            binary_free(&sdsbuf);
            *size = 0;
            return NULL;
        }
    }
    _redis_create_sds(&fbuf, &sdsbuf, &n, &pending);
    /* 格式化 RESP 数组计数头并回填到 sdsbuf 头部预留槽中。
     * memmove 将 Bulk String 主体向左移动以消除填充间隙，
     * 避免额外的输出内存分配。 */
    int hlen_int = SNPRINTF(_fmt, sizeof(_fmt), "*%zu"FLAG_CRLF, n);
    size_t hlens = snprintf_lens(hlen_int, sizeof(_fmt));
    ASSERTAB(sdsbuf.offset >= MAX_HEADER_RESERVE, "RESP body skip violated.");
    size_t body_size = sdsbuf.offset - MAX_HEADER_RESERVE;
    ASSERTAB(hlens <= MAX_HEADER_RESERVE, "RESP header too long for reserved slot.");
    memmove(sdsbuf.data + hlens, sdsbuf.data + MAX_HEADER_RESERVE, body_size);
    memcpy(sdsbuf.data, _fmt, hlens);
    *size = hlens + body_size;
    /* *size < MAX_HEADER_RESERVE + body_size == sdsbuf.offset <= sdsbuf.size */
    sdsbuf.data[*size] = '\0';
    binary_free(&fbuf);
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
static inline reader_ctx *_redis_create_reader(ud_cxt *ud) {
    if (NULL == ud->context) {
        reader_ctx *rd;
        MALLOC(rd, sizeof(reader_ctx));
        rd->count = 0;
        rd->head = NULL;
        rd->tail = NULL;
        rd->depth = 1; // 顶层虚拟帧，期望 1 个顶层元素
        rd->stack[0].remain = 1;
        rd->stack[0].attr = 0;
        ud->context = rd;
    }
    return ud->context;
}
// 将已解析节点尾插到链表（节点由 CALLOC 分配，next 已为 NULL），并消费帧栈上的一格。
// open > 0 为声明了元素的聚合类型：压入新层，父层的这个元素要等它闭合才算完成；
// 否则为叶子或空聚合：消费栈顶一格，栈顶归零则逐层弹出并级联消费父层（ATTR 层闭合不消费父层）
static inline void _redis_add_node(reader_ctx *rd, redis_pack_ctx *pk, int64_t open) {
    if (NULL == rd->tail) {
        rd->head = pk;
    } else {
        rd->tail->next = pk;
    }
    rd->tail = pk;
    rd->count++;
    if (open > 0) {
        ASSERTAB(rd->depth < REDIS_MAX_DEPTH, "redis frame stack overflow.");
        rd->stack[rd->depth].remain = open;
        rd->stack[rd->depth].attr = (RESP_ATTR == pk->prot);
        rd->depth++;
        return;
    }
    if (RESP_ATTR == pk->prot) {
        return;
    }
    while (rd->depth > 0) {
        rd->stack[rd->depth - 1].remain--;
        if (rd->stack[rd->depth - 1].remain > 0) {
            break;
        }
        rd->depth--;
        if (rd->stack[rd->depth].attr) {
            break;
        }
    }
}
// 定位下一个 CRLF；未找到时按已累积字节数区分"数据不足"与"行首长度已超限"，置 status 并返回 ERR_FAILED
static inline int32_t _redis_find_crlf(buffer_ctx *buf, int32_t *status) {
    int32_t pos = buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE);
    if (ERR_FAILED == pos) {
        if (buffer_size(buf) > REDIS_MAX_LINE_LENS) {
            BIT_SET(*status, PROT_ERROR);
        } else {
            BIT_SET(*status, PROT_MOREDATA);
        }
    }
    return pos;
}
// 解析长度行(CRLF 位于 pos)紧邻的数值 token为 >= -1 的整数；容量不够/非纯数字/溢出 int64 时置 PROT_ERROR
static inline int32_t _redis_parse_len(buffer_ctx *buf, int32_t pos, int32_t *status, int64_t *out) {
    char num[64];
    int32_t lens = pos - 1;
    if (lens <= 0 || lens >= (int32_t)sizeof(num)) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    buffer_copyout(buf, 1, num, lens);
    // 长度严格按 ['-']1*DIGIT 解析(允许 null 的 "-1"),前导空白与 '+' 一律拒,免得与对端切出不同的包边界。
    // RESP_INTEGER 的 ':' 本就允许 '+'(见 redis.h),不走这里
    int64_t val;
    if (ERR_OK != parse_int64_strict(num, (size_t)lens, &val)
        || val < -1) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    *out = val;
    return ERR_OK;
}
// 解析单行类型（简单字符串/错误/整数/空值/布尔/浮点/大整数）：格式 <type><data>\r\n
static int32_t _redis_reader_line(reader_ctx *rd, int32_t prot, buffer_ctx *buf, int32_t *status) {
    int32_t pos = _redis_find_crlf(buf, status);
    if (ERR_FAILED == pos) {
        return ERR_FAILED;
    }
    redis_pack_ctx *pk;
    CALLOC(pk, 1, sizeof(redis_pack_ctx) + pos);//前面还有1个type字节
    pk->prot = prot;
    // pos 是 CRLF 的偏移。能进本函数的首字节必是 +-:_#,( 之一(见 redis_unpack 的 switch),
    // 不可能是 '\r',故 pos >= 1、len 不可能为负
    pk->len = pos - 1;
    switch (prot) {
    case RESP_STRING:
    case RESP_ERROR:
        buffer_copyout(buf, 1, pk->data, (size_t)pk->len);
        break;
    case RESP_INTEGER:
        if (0 == pk->len) {
            BIT_SET(*status, PROT_ERROR);
        } else {
            buffer_copyout(buf, 1, pk->data, (size_t)pk->len);
            char *end;
            errno = 0;
            pk->ival = strtoll(pk->data, &end, 10);
            if (end != pk->data + pk->len
                || errno == ERANGE) {
                BIT_SET(*status, PROT_ERROR);
            }
        }
        break;
    case RESP_BIGNUM:// 任意精度,原样保留 data 字符串交业务解析(不限 int64,不解析 ival)
        if (0 == pk->len) {
            BIT_SET(*status, PROT_ERROR);
        } else {
            buffer_copyout(buf, 1, pk->data, (size_t)pk->len);
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
        if ('t' != pk->data[0] && 'T' != pk->data[0]
            && 'f' != pk->data[0] && 'F' != pk->data[0]) {
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
        if (3 == pk->len && 0 == memcasecmp(pk->data, "inf", (size_t)pk->len)) {
            pk->dval = INFINITY;
        } else if (4 == pk->len && 0 == memcasecmp(pk->data, "-inf", (size_t)pk->len)) {
            pk->dval = -INFINITY;
        } else if ((3 == pk->len && 0 == memcasecmp(pk->data, "nan", (size_t)pk->len))
                   ||(4 == pk->len && 0 == memcasecmp(pk->data, "-nan", (size_t)pk->len))) {
            pk->dval = NAN;
        } else {
            char *end;
            pk->dval = strtod_c(pk->data, &end);
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
    _redis_add_node(rd, pk, 0);
    return ERR_OK;
}
// 解析批量字符串类型（Bulk String/Error/Verbatim）：格式 <type><length>\r\n<data>\r\n，长度为 -1 表示 Null
static int32_t _redis_reader_bulk(reader_ctx *rd, int32_t prot, buffer_ctx *buf, int32_t *status) {
    int32_t pos = _redis_find_crlf(buf, status);
    if (ERR_FAILED == pos) {
        return ERR_FAILED;
    }
    int64_t blens;
    if (ERR_OK != _redis_parse_len(buf, pos, status, &blens)) {
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
        _redis_add_node(rd, pk, 0);
        return ERR_OK;
    }
    if (blens > REDIS_MAX_BULK_LENS) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
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
    _redis_add_node(rd, pk, 0);
    return ERR_OK;
}
// 解析聚合类型（数组/集合/推送/映射/属性）：格式 <type><number-of-elements>\r\n<element-1>...<element-n>
static int32_t _redis_reader_agg(reader_ctx *rd, int32_t prot, buffer_ctx *buf, int32_t *status) {
    int32_t pos = _redis_find_crlf(buf, status);
    if (ERR_FAILED == pos) {
        return ERR_FAILED;
    }
    int64_t nelem;
    if (ERR_OK != _redis_parse_len(buf, pos, status, &nelem)) {
        return ERR_FAILED;
    }
    if (nelem > (int64_t)INT32_MAX) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    int64_t open = (nelem > 0) ? ((RESP_ATTR == prot || RESP_MAP == prot) ? nelem * 2 : nelem) : 0;
    if (open > 0
        && rd->depth >= REDIS_MAX_DEPTH) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    size_t del = (size_t)(pos + CRLF_SIZE);
    ASSERTAB(del == buffer_drain(buf, del), "drain buffer failed.");
    redis_pack_ctx *pk;
    CALLOC(pk, 1, sizeof(redis_pack_ctx) + 1);
    pk->prot = prot;
    pk->nelem = nelem;
    _redis_add_node(rd, pk, open);
    return ERR_OK;
}
void *redis_unpack(struct ev_ctx *ev, sock_ctx *sk, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status) {
    (void)ev; (void)sk; (void)client; (void)size;
    int32_t rtn, prot;
    redis_pack_ctx *pk;
    reader_ctx *rd = _redis_create_reader(ud);
    for (;;) {
        if (rd->count >= REDIS_MAX_NODES) {
            BIT_SET(*status, PROT_ERROR);
            break;
        }
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
            rtn = _redis_reader_line(rd, prot, buf, status);
            break;
        case RESP_BSTRING:
        case RESP_BERROR:
        case RESP_VERB:
            rtn = _redis_reader_bulk(rd, prot, buf, status);
            break;
        case RESP_ARRAY:
        case RESP_SET:
        case RESP_PUSHE:
        case RESP_MAP:
        case RESP_ATTR:
            rtn = _redis_reader_agg(rd, prot, buf, status);
            break;
        default:
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        if (ERR_OK != rtn) {
            break;
        }
        if (0 == rd->depth) {
            pk = rd->head;
            rd->head = NULL;
            rd->tail = NULL;
            rd->count = 0;
            rd->depth = 1;
            rd->stack[0].remain = 1;
            rd->stack[0].attr = 0;
            return pk;
        }
    }
    return NULL;
}
