#include "protocol/redis.h"
#include "utils/binary.h"

/* RESP 数组计数头 "*n\r\n" 所需的最大字节数。
 * 即使 "*999999\r\n" 也只有 10 字节；32 字节预留足够裕量。 */
#define MAX_HEADER_RESERVE  32
//单次 RESP 解包最大节点数，纯内存兜底；正确性由 REDIS_MAX_DEPTH 保证，
//此值只决定为一个未完成回复最多垫多少内存，不应低到误伤合法的扁平大结果集。
//1<<18 时最坏约 17MB（节点约 64B/个），仍是现有 7 万元素用例的 3.7 倍余量；
//再往上抬只是让恶意 server 能用约 4MB 流量顶出几十 MB，业务侧的大结果集应分页取
#define REDIS_MAX_NODES     (1 << 18)
#define REDIS_SOLO_NODES    4 // 一条回复的前几个节点单独分配，其后的从块链切：小聚合开一整块反而比几次小分配贵
#define REDIS_NUM_CAP       64 // 长度 token 的容量(不含)：最多 63 位
#define REDIS_HDR_PEEK      (REDIS_NUM_CAP + 2) // 元素头本地副本：类型 1 + 长度最多 63 位 + CRLF 2
#define FMT_INTEGER_FLAG  "diouxX" // 整型格式字符集
// 提取 [p, f] 范围的格式说明符，格式化并追加到 fbuf
#define FMT_TYPE(type)\
    lens = (size_t)(f - p) + 1;\
    ASSERTAB(lens < sizeof(_fmt), "redis format specifier too long");\
    memcpy(_fmt, p, lens);\
    _fmt[lens] = '\0';\
    binary_set_va(&fbuf, _fmt, va_arg(args, type))
// 整数转换：裸 %d %i %u(见 _redis_fmt_bare)直接转十进制，输出与 printf 相同；其余照旧走 FMT_TYPE。
// stype / utype 为这个长度修饰下 printf 实际按哪个有符号 / 无符号类型取值
#define FMT_INT(type, stype, utype)\
    if (_redis_fmt_bare(p, f) && ('d' == *f || 'i' == *f || 'u' == *f)) {\
        type _iv = va_arg(args, type);\
        if ('u' == *f) {\
            binary_set_uint(&fbuf, (uint64_t)(utype)_iv, 10);\
        } else {\
            binary_set_int(&fbuf, (int64_t)(stype)_iv, 10);\
        }\
    } else {\
        FMT_TYPE(type);\
    }

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
    size_t bytes;           // 当前回复各节点的分配长度之和，回复完整时交给 *size 并清零
    redis_frame stack[REDIS_MAX_DEPTH];
    mem_arena arena;        // 当前回复第 REDIS_SOLO_NODES 个之后的节点从这里切，回复完整时挂到首节点 blocks 上
}reader_ctx;

// 前 REDIS_SOLO_NODES 个节点单独分配(见 _redis_node_new)，逐个放；其余在首节点的块链上，整链放。
// 有 next 的首节点必是聚合类型，blocks 才有意义(与 dval 共用一格)
void _redis_pkfree(void *data) {
    redis_pack_ctx *pack = (redis_pack_ctx *)data;
    if (NULL == pack) {
        return;
    }
    if (NULL == pack->next) {
        FREE(pack);
        return;
    }
    mem_arena arena = { pack->blocks, 0, 0 };
    redis_pack_ctx *next;
    int32_t i;
    for (i = 0; i < REDIS_SOLO_NODES && NULL != pack; i++) {
        next = pack->next;
        FREE(pack);
        pack = next;
    }
    mem_arena_free(&arena);
}
void _redis_udfree(ud_cxt *ud) {
    if (NULL == ud->context) {
        return;
    }
    reader_ctx *rd = ud->context;
    _redis_pkfree(rd->head);// 未完成回复里已解析的节点
    mem_arena_free(&rd->arena);
    FREE(rd);
    ud->context = NULL;
}
// 把一段数据作为一个 RESP Bulk String 追加到 sdsbuf，n 自增
static inline void _redis_put_bulk(binary_ctx *sdsbuf, const char *data, size_t lens, size_t *n) {
    binary_set_binary(sdsbuf, "$", 1);
    binary_set_uint(sdsbuf, (uint64_t)lens, 10);
    binary_set_binary(sdsbuf, FLAG_CRLF, CRLF_SIZE);
    if (lens > 0) {
        binary_set_binary(sdsbuf, data, lens);
    }
    binary_set_binary(sdsbuf, FLAG_CRLF, CRLF_SIZE);
    (*n)++;
}
// pending 非 0（当前位置存在参数，含空串）时将 fbuf 内容作为一个 RESP Bulk String 追加到 sdsbuf（空参数输出 $0）；重置 fbuf 偏移与 pending，n 自增
static inline void _redis_create_sds(binary_ctx *fbuf, binary_ctx *sdsbuf, size_t *n, int32_t *pending) {
    if (0 == *pending) {
        return;
    }
    _redis_put_bulk(sdsbuf, fbuf->data, fbuf->offset, n);
    binary_offset(fbuf, 0);
    *pending = 0;
}
// [p, f] 这段转换说明是否只有 '%'、可选的 l / ll / z 长度修饰和转换符，不带标志、宽度、精度
static inline int32_t _redis_fmt_bare(const char *p, const char *f) {
    size_t mods = (size_t)(f - p) - 1;
    return 0 == mods
        || (1 == mods && ('l' == p[1] || 'z' == p[1]))
        || (2 == mods && 'l' == p[1] && 'l' == p[2]);
}
// p 处的两字符转换(%s / %b)是否单独成一个参数：前面是串首或空格、fbuf 里还没攒东西、后面是空格或串尾。
// 是的话参数直接写进输出，不经 fbuf 过一手
static inline int32_t _redis_fmt_alone(const char *fmt, const char *p, binary_ctx *fbuf) {
    return 0 == fbuf->offset
        && (p == fmt || ' ' == p[-1])
        && (' ' == p[2] || '\0' == p[2]);
}
// redis_pack 的内部实现，解析格式字符串并将各参数编码为 RESP Bulk String 序列。
// 裸 %s 直接拷不过 vsnprintf，实参 NULL 照各家 vsnprintf 的做法写 "(null)"
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
            val = va_arg(args, char *);
            if (NULL == val) {
                val = "(null)";
            }
            if (_redis_fmt_alone(fmt, p, &fbuf)) {
                _redis_put_bulk(&sdsbuf, val, strlen(val), &n);
                pending = 0;
                f += ('\0' == f[1]) ? 1 : 2;
                continue;
            }
            binary_set_binary(&fbuf, val, strlen(val));
            f++;
            break;
        }
        case 'b': {
            val = va_arg(args, char *);
            lens = va_arg(args, size_t);
            if (_redis_fmt_alone(fmt, p, &fbuf)) {
                _redis_put_bulk(&sdsbuf, val, lens, &n);
                pending = 0;
                f += ('\0' == f[1]) ? 1 : 2;
                continue;
            }
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
                FMT_INT(int, int, unsigned int);
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
                    FMT_INT(long long, long long, unsigned long long);
                    f++;
                } else {// 长度修饰后面不是整数转换
                    fmterr = 1;
                }
                break;
            }
            if ('l' == *f) {
                f++;
                if ('\0' != *f && NULL != strchr(FMT_INTEGER_FLAG, *f)) {
                    FMT_INT(long, long, unsigned long);
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
                    FMT_INT(size_t, ptrdiff_t, size_t);
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
    char num[INT2STR_MAX];
    size_t nlens = u64tostr(num, (uint64_t)n, 10);
    size_t hlens = 1 + nlens + CRLF_SIZE;
    ASSERTAB(sdsbuf.offset >= MAX_HEADER_RESERVE, "RESP body skip violated.");
    size_t body_size = sdsbuf.offset - MAX_HEADER_RESERVE;
    ASSERTAB(hlens <= MAX_HEADER_RESERVE, "RESP header too long for reserved slot.");
    memmove(sdsbuf.data + hlens, sdsbuf.data + MAX_HEADER_RESERVE, body_size);
    sdsbuf.data[0] = '*';
    memcpy(sdsbuf.data + 1, num, nlens);
    memcpy(sdsbuf.data + 1 + nlens, FLAG_CRLF, CRLF_SIZE);
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
        rd->bytes = 0;
        rd->depth = 1; // 顶层虚拟帧，期望 1 个顶层元素
        rd->stack[0].remain = 1;
        rd->stack[0].attr = 0;
        ZERO(&rd->arena, sizeof(rd->arena));
        ud->context = rd;
    }
    return ud->context;
}
// 新节点，只清结构体头(data 由调用方写，含结尾 '\0')。回复的前 REDIS_SOLO_NODES 个节点单独分配，
// 之后的从 rd 的块链切，不能单独释放
static inline redis_pack_ctx *_redis_node_new(reader_ctx *rd, size_t lens) {
    redis_pack_ctx *pk;
    if (rd->count < REDIS_SOLO_NODES) {
        MALLOC(pk, lens);
    } else {
        pk = mem_arena_alloc(&rd->arena, lens);
    }
    rd->bytes += lens;
    ZERO(pk, sizeof(redis_pack_ctx));
    return pk;
}
// 丢弃还没挂上链表的新节点：单独分配的放掉，块链里的随 rd 一起还
static inline void _redis_node_drop(reader_ctx *rd, redis_pack_ctx *pk) {
    if (rd->count < REDIS_SOLO_NODES) {
        FREE(pk);
    }
}
// 将已解析节点尾插到链表（next 已为 NULL），并消费帧栈上的一格。
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
// 在元素头的本地副本里找首个 CRLF 对；首字节是类型字节不会是 '\r'，从 1 起找
static inline int32_t _redis_local_crlf(const char *hdr, size_t n) {
    size_t i;
    for (i = 1; i + 1 < n; i++) {
        if ('\r' == hdr[i] && '\n' == hdr[i + 1]) {
            return (int32_t)i;
        }
    }
    return ERR_FAILED;
}
// 解析 RESP_INTEGER 的值。收的范围与 strtoll(base 10) + 必须吃完整段 + 不溢出 完全一致：
// 前导空白与一个可选 '+' 照收("+-5" 照拒)，数字部分交 strtoi64
static inline int32_t _redis_parse_integer(const char *p, size_t lens, int64_t *out) {
    const char *end = p + lens;
    while (p < end && isspace((unsigned char)*p)) {
        p++;
    }
    if (p < end && '+' == *p) {
        p++;
        if (p < end && '-' == *p) {
            return ERR_FAILED;
        }
    }
    return strtoi64(p, (size_t)(end - p), out);
}
// 把长度 token 严格按 ['-']1*DIGIT 解析为 >= -1 的整数；容量不够/非纯数字/溢出 int64 时置 PROT_ERROR
static inline int32_t _redis_parse_num(const char *num, int32_t lens, int32_t *status, int64_t *out) {
    if (lens <= 0 || lens >= REDIS_NUM_CAP) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    // 前导空白与 '+' 一律拒,免得与对端切出不同的包边界。RESP_INTEGER 的 ':' 本就允许 '+'(见 redis.h),不走这里
    int64_t val;
    if (ERR_OK != strtoi64(num, (size_t)lens, &val)
        || val < -1) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    }
    *out = val;
    return ERR_OK;
}
// 解析长度行(CRLF 位于 pos)紧邻的数值 token
static inline int32_t _redis_parse_len(buffer_ctx *buf, int32_t pos, int32_t *status, int64_t *out) {
    char num[REDIS_NUM_CAP];
    int32_t lens = pos - 1;
    if (lens > 0 && lens < (int32_t)sizeof(num)) {
        buffer_copyout(buf, 1, num, lens);
    }
    return _redis_parse_num(num, lens, status, out);
}
// bulk / 聚合的长度行：本地副本里找到 CRLF 就地解析，找不到走整缓冲查找（结果与之前逐字相同）
static inline int32_t _redis_head_len(buffer_ctx *buf, const char *hdr, size_t n,
    int32_t *status, int32_t *pos, int64_t *out) {
    *pos = _redis_local_crlf(hdr, n);
    if (ERR_FAILED != *pos) {
        return _redis_parse_num(hdr + 1, *pos - 1, status, out);
    }
    *pos = _redis_find_crlf(buf, status);
    if (ERR_FAILED == *pos) {
        return ERR_FAILED;
    }
    return _redis_parse_len(buf, *pos, status, out);
}
// 解析单行类型（简单字符串/错误/整数/空值/布尔/浮点/大整数）：格式 <type><data>\r\n。
// 数据段先统一拷出再按类型校验，STRING / ERROR 就是原样保留；行在 hdr 本地副本里就不再碰 buf
static int32_t _redis_reader_line(reader_ctx *rd, int32_t prot, buffer_ctx *buf,
    const char *hdr, size_t n, int32_t *status) {
    int32_t pos = _redis_local_crlf(hdr, n);
    int32_t local = (ERR_FAILED != pos);
    if (!local) {
        pos = _redis_find_crlf(buf, status);
        if (ERR_FAILED == pos) {
            return ERR_FAILED;
        }
    }
    redis_pack_ctx *pk = _redis_node_new(rd, sizeof(redis_pack_ctx) + pos);//前面还有1个type字节
    pk->prot = prot;
    // pos 是 CRLF 的偏移。能进本函数的首字节必是 +-:_#,( 之一(见 redis_unpack 的 switch),
    // 不可能是 '\r',故 pos >= 1、len 不可能为负
    pk->len = pos - 1;
    if (pk->len > 0) {
        if (local) {
            memcpy(pk->data, hdr + 1, (size_t)pk->len);
        } else {
            buffer_copyout(buf, 1, pk->data, (size_t)pk->len);
        }
    }
    pk->data[pk->len] = '\0';
    switch (prot) {
    case RESP_INTEGER:
        if (0 == pk->len) {
            BIT_SET(*status, PROT_ERROR);
        } else {
            if (ERR_OK != _redis_parse_integer(pk->data, (size_t)pk->len, &pk->ival)) {
                BIT_SET(*status, PROT_ERROR);
            }
        }
        break;
    case RESP_BIGNUM:// 任意精度,原样保留 data 字符串交业务解析(不限 int64,不解析 ival)
        if (0 == pk->len) {
            BIT_SET(*status, PROT_ERROR);
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
        _redis_node_drop(rd, pk);
        return ERR_FAILED;
    }
    int32_t del = pos + CRLF_SIZE;
    ASSERTAB(del == (int32_t)buffer_drain(buf, del), "drain buffer failed.");
    _redis_add_node(rd, pk, 0);
    return ERR_OK;
}
// 解析批量字符串类型（Bulk String/Error/Verbatim）：格式 <type><length>\r\n<data>\r\n，长度为 -1 表示 Null。
// 数据连同结尾 CRLF 一次拷出再本地校验，故块多留 CRLF 两字节；整段已在 hdr 本地副本里就不再碰 buf，
// 尾 CRLF 也在 hdr 里查，不去读刚拷完的目标块
static int32_t _redis_reader_bulk(reader_ctx *rd, int32_t prot, buffer_ctx *buf,
    const char *hdr, size_t n, int32_t *status) {
    int32_t pos;
    int64_t blens;
    if (ERR_OK != _redis_head_len(buf, hdr, n, status, &pos, &blens)) {
        return ERR_FAILED;
    }
    size_t total;
    if (-1 == blens) {
        redis_pack_ctx *pk = _redis_node_new(rd, sizeof(redis_pack_ctx) + 1);
        pk->data[0] = '\0';
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
    size_t doff = (size_t)(pos + CRLF_SIZE);
    size_t dcopy = (size_t)blens + CRLF_SIZE;
    redis_pack_ctx *pk = _redis_node_new(rd, sizeof(redis_pack_ctx) + dcopy);
    pk->prot = prot;
    const char *tail;
    if (total <= n) {
        memcpy(pk->data, hdr + doff, dcopy);
        tail = hdr + doff + blens;
    } else {
        buffer_copyout(buf, doff, pk->data, dcopy);
        tail = pk->data + blens;
    }
    if ('\r' != tail[0]
        || '\n' != tail[1]) {
        BIT_SET(*status, PROT_ERROR);
    } else if (RESP_VERB == prot) {
        if (blens < 4
            || ':' != pk->data[3]) {
            BIT_SET(*status, PROT_ERROR);
        } else {
            memcpy(pk->venc, pk->data, 3);//3 bytes encoding
            pk->len = blens - 4;
            memmove(pk->data, pk->data + 4, (size_t)pk->len);
        }
    } else {
        pk->len = blens;
    }
    if (BIT_CHECK(*status, PROT_ERROR)) {
        _redis_node_drop(rd, pk);
        return ERR_FAILED;
    }
    pk->data[pk->len] = '\0';
    ASSERTAB(total == buffer_drain(buf, total), "drain buffer failed.");
    _redis_add_node(rd, pk, 0);
    return ERR_OK;
}
// 解析聚合类型（数组/集合/推送/映射/属性）：格式 <type><number-of-elements>\r\n<element-1>...<element-n>
static int32_t _redis_reader_agg(reader_ctx *rd, int32_t prot, buffer_ctx *buf,
    const char *hdr, size_t n, int32_t *status) {
    int32_t pos;
    int64_t nelem;
    if (ERR_OK != _redis_head_len(buf, hdr, n, status, &pos, &nelem)) {
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
    redis_pack_ctx *pk = _redis_node_new(rd, sizeof(redis_pack_ctx) + 1);
    pk->data[0] = '\0';
    pk->prot = prot;
    pk->nelem = nelem;
    _redis_add_node(rd, pk, open);
    return ERR_OK;
}
void *redis_unpack(struct ev_ctx *ev, sock_ctx *sk, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status) {
    (void)ev; (void)sk; (void)client;
    int32_t rtn, prot;
    size_t n;
    char hdr[REDIS_HDR_PEEK];
    redis_pack_ctx *pk;
    reader_ctx *rd = _redis_create_reader(ud);
    for (;;) {
        if (rd->count >= REDIS_MAX_NODES) {
            BIT_SET(*status, PROT_ERROR);
            break;
        }
        n = buffer_size(buf);
        if (n < (1 + CRLF_SIZE)) {
            BIT_SET(*status, PROT_MOREDATA);
            break;
        }
        if (n > sizeof(hdr)) {
            n = sizeof(hdr);
        }
        buffer_copyout(buf, 0, hdr, n);
        prot = hdr[0];
        switch (prot) {
        case RESP_STRING:
        case RESP_ERROR:
        case RESP_INTEGER:
        case RESP_NIL:
        case RESP_BOOL:
        case RESP_DOUBLE:
        case RESP_BIGNUM:
            rtn = _redis_reader_line(rd, prot, buf, hdr, n, status);
            break;
        case RESP_BSTRING:
        case RESP_BERROR:
        case RESP_VERB:
            rtn = _redis_reader_bulk(rd, prot, buf, hdr, n, status);
            break;
        case RESP_ARRAY:
        case RESP_SET:
        case RESP_PUSHE:
        case RESP_MAP:
        case RESP_ATTR:
            rtn = _redis_reader_agg(rd, prot, buf, hdr, n, status);
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
            if (NULL != rd->arena.cur) {
                pk->blocks = rd->arena.cur;// 整条块链从当前块串起
                ZERO(&rd->arena, sizeof(rd->arena));
            }
            *size = rd->bytes;
            rd->head = NULL;
            rd->tail = NULL;
            rd->bytes = 0;
            rd->count = 0;
            rd->depth = 1;
            rd->stack[0].remain = 1;
            rd->stack[0].attr = 0;
            return pk;
        }
    }
    return NULL;
}
