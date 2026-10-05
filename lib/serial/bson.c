#include "serial/bson.h"

// bson_cat / 迭代器 / bson_check_depth 三处校验严在不同的轴上, 别拿一处的宽松当漏检去补齐。
// 最易踩: 提前出现 EOD 后还剩没用掉的字节, 迭代器当遍历正常结束静默忽略, 只有 bson_check_depth 拒

// 迭代器推进失败的哨兵。doclens 已由 bson_iter_init 校验不超过 buffer 大小,
// 真实 offset 取不到这个值
#define ITER_BAD ((size_t)-1)

static char _bson_empty[5] = { 0 };
static uint8_t _oid_header[5];
static atomic_t _oid_counter = 0;

// 初始化 OID 头部：由主机名哈希（3字节）和 PID（2字节）组成，并设置计数器初始值
static void _bson_oid_init(void) {
    int32_t pid = GETPID();
    uint32_t h = 0;
    char hostname[HOST_LENS] = { 0 };
    if (0 == gethostname(hostname, sizeof(hostname))) {
        int32_t i;
        for (i = 0; i < (int32_t)sizeof(hostname) && hostname[i]; i++) {
            h = h ^ ((h << 5) + (h >> 2) + hostname[i]);
        }
        h ^= i;
    }
    pack_integer((char *)_oid_header, h, 3, 1);
    write_le16(_oid_header + 3, (uint16_t)pid);
    _oid_counter = randrange(10000, 20000);
}
// 初始化静态空 BSON 文档（长度为5：4字节长度 + 1字节 EOD）
static void _bson_empty_init(void) {
    write_le32(_bson_empty, 5);
}
void bson_globle_init(void) {
    _bson_oid_init();
    _bson_empty_init();
}
void bson_oid(char oid[BSON_OID_LENS]) {
    time_t ti = time(NULL);
    uint32_t id = ATOMIC_ADD_RELAXED(&_oid_counter, 1);
    write_be32(oid, (uint32_t)ti);
    memcpy(oid + 4, _oid_header, 5);
    pack_integer(oid + 9, id, 3, 0);
}
const char *bson_empty(size_t *lens) {
    SET_PTR(lens, 5);
    return _bson_empty;
}
// 开始一个新的嵌套层级：增加层级计数，记录当前偏移，预留4字节用于回填长度
static inline void _bson_append_start(bson_ctx *bson) {
    bson->depth++;
    ASSERTAB(bson->depth <= BSON_MAX_DEPTH, "too much depth.");
    bson->offsets[bson->depth - 1] = bson->doc.offset;
    binary_set_skip(&bson->doc, 4);
}
// src 可能指向本 ctx 自己的缓冲(key 与各 append 的值参数都可能取自 BSON_DOC(bson) 区间),
// 规则同 binary_set_binary:扩容会把它搬走,扩容前记下标,扩容后按下标重取
static inline uintptr_t _bson_src_mark(bson_ctx *bson, const void *src, int32_t *inner) {
    uintptr_t off = (uintptr_t)src - (uintptr_t)bson->doc.data;
    *inner = (NULL != bson->doc.data && off < bson->doc.size);
    return off;
}
// 一次扩够 type + key + NUL + vlens 字节并写好 type 与 key,返回值区起点。key 同样可能指向本 ctx 的缓冲;
// 只拷 klens 字节、结尾 NUL 自己补,key 不必以 NUL 结尾
static inline char *_bson_append_head(bson_ctx *bson, bson_type type, const char *key, size_t klens, size_t vlens) {
    int32_t inner;
    uintptr_t koff = _bson_src_mark(bson, key, &inner);
    size_t start = bson->doc.offset;
    binary_set_skip(&bson->doc, 1 + klens + 1 + vlens);
    char *p = bson->doc.data + start;
    p[0] = (char)type;
    memmove(p + 1, inner ? bson->doc.data + koff : key, klens);
    p[1 + klens] = '\0';
    return p + 2 + klens;
}
// 子文档/数组的开头:key 与 4 字节长度占位一次扩够,并记下长度字段的位置
static inline void _bson_append_sub(bson_ctx *bson, bson_type type, const char *key, size_t klens) {
    char *p = _bson_append_head(bson, type, key, klens, 4);
    bson->depth++;
    ASSERTAB(bson->depth <= BSON_MAX_DEPTH, "too much depth.");
    bson->offsets[bson->depth - 1] = (size_t)(p - bson->doc.data);
}
// 带 int32 长度前缀、结尾补 NUL 的字符串值(utf8 / jscode)
static inline void _bson_append_str(bson_ctx *bson, bson_type type, const char *key, size_t klens,
                                    const char *val, size_t lens) {
    int32_t inner;
    uintptr_t off = _bson_src_mark(bson, val, &inner);
    char *p = _bson_append_head(bson, type, key, klens, 4 + lens + 1);
    write_le32(p, (uint32_t)(lens + 1));
    if (lens > 0) {
        memmove(p + 4, inner ? bson->doc.data + off : val, lens);
    }
    p[4 + lens] = '\0';
}
// 原样拷入一段已编好的子文档/数组
static inline void _bson_append_raw(bson_ctx *bson, bson_type type, const char *key, const char *doc, size_t lens) {
    int32_t inner;
    uintptr_t off = _bson_src_mark(bson, doc, &inner);
    char *p = _bson_append_head(bson, type, key, strlen(key), (NULL == doc) ? 0 : lens);
    if (NULL != doc && lens > 0) {
        memmove(p, inner ? bson->doc.data + off : doc, lens);
    }
}
void bson_init(bson_ctx *bson, char *data, size_t lens) {
    bson->depth = 0;
    if (NULL == data) {
        //构建模式：自己开缓冲,并预留顶层文档的 4 字节长度前缀
        binary_init_write(&bson->doc, lens, 0);
        _bson_append_start(bson);
    } else {
        binary_init_read(&bson->doc, data, lens);
    }
}
void bson_init_prefix(bson_ctx *bson, size_t lens, size_t prefix) {
    bson->depth = 0;
    binary_init_write(&bson->doc, lens + prefix, 0);
    binary_set_skip(&bson->doc, prefix);
    _bson_append_start(bson);
}
int32_t bson_complete(bson_ctx *bson) {
    return 0 == bson->depth && bson->doc.offset > 0;
}
void bson_append_end(bson_ctx *bson) {
    ASSERTAB(bson->depth > 0, "logic error.");
    binary_set_int8(&bson->doc, BSON_EOD);
    size_t endoff = bson->doc.offset;
    size_t startoff = bson->offsets[bson->depth - 1];
    ASSERTAB(endoff - startoff <= INT32_MAX, "BSON document length exceeds 2GB limit");
    write_le32(bson->doc.data + startoff, (uint32_t)(endoff - startoff));
    bson->depth--;
}
int32_t bson_cat(bson_ctx *bson, char *doc, size_t lens) {
    if (NULL == doc
        || 0 == lens) {
        return ERR_OK;
    }
    if (lens < 5) {
        return ERR_FAILED;
    }
    uint32_t doclens = read_le32(doc);
    if ((size_t)doclens > lens) {
        return ERR_FAILED;
    }
    // 声明长度 0~4 结构上不可能(最短的空文档是 4 字节长度 + EOD = 5),不是 no-op 而是畸形
    if (doclens < 5) {
        return ERR_FAILED;
    }
    // 下面按"最后一字节是 EOD"把它砍掉,那就得先确认它真是 EOD;否则非 BSON 输入会被静默拼进来
    if (0 != doc[doclens - 1]) {
        return ERR_FAILED;
    }
    if (5 == doclens) {
        return ERR_OK;// 空文档,没有字段可拼
    }
    binary_set_binary(&bson->doc, doc + 4, doclens - 5);//4 + 1(eod)
    return ERR_OK;
}
// 带 2 的版本由调用方给出 key 的字节数，不再 strlen；不带 2 的转调它们
void bson_append_document_begain2(bson_ctx *bson, const char *key, size_t klens) {
    _bson_append_sub(bson, BSON_DOCUMENT, key, klens);
}
void bson_append_document_begain(bson_ctx *bson, const char *key) {
    bson_append_document_begain2(bson, key, strlen(key));
}
void bson_append_array_begain2(bson_ctx *bson, const char *key, size_t klens) {
    _bson_append_sub(bson, BSON_ARRAY, key, klens);
}
void bson_append_array_begain(bson_ctx *bson, const char *key) {
    bson_append_array_begain2(bson, key, strlen(key));
}
//signed_byte(1) e_name double
void bson_append_double2(bson_ctx *bson, const char *key, size_t klens, double val) {
    pack_double(_bson_append_head(bson, BSON_DOUBLE, key, klens, sizeof(double)), val, 1);
}
void bson_append_double(bson_ctx *bson, const char *key, double val) {
    bson_append_double2(bson, key, strlen(key), val);
}
//signed_byte(2) e_name string
void bson_append_utf8_n2(bson_ctx *bson, const char *key, size_t klens, const char *val, size_t lens) {
    ASSERTAB(lens <= INT32_MAX - 1, "BSON UTF-8 string length exceeds 2GB limit");
    _bson_append_str(bson, BSON_UTF8, key, klens, val, lens);
}
void bson_append_utf8_n(bson_ctx *bson, const char *key, const char *val, size_t lens) {
    bson_append_utf8_n2(bson, key, strlen(key), val, lens);
}
void bson_append_utf8(bson_ctx *bson, const char *key, const char *val) {
    bson_append_utf8_n(bson, key, val, strlen(val));
}
//signed_byte(3) e_name document
void bson_append_document(bson_ctx *bson, const char *key, char *doc, size_t lens) {
    _bson_append_raw(bson, BSON_DOCUMENT, key, doc, lens);
}
//signed_byte(4) e_name document
void bson_append_array(bson_ctx *bson, const char *key, char *doc, size_t lens) {
    _bson_append_raw(bson, BSON_ARRAY, key, doc, lens);
}
//signed_byte(5) e_name binary
void bson_append_binary2(bson_ctx *bson, const char *key, size_t klens, bson_subtype type, char *val, size_t lens) {
    ASSERTAB(lens <= INT32_MAX, "BSON binary length exceeds 2GB limit");
    int32_t inner;
    uintptr_t off = _bson_src_mark(bson, val, &inner);
    char *p = _bson_append_head(bson, BSON_BINARY, key, klens, 4 + 1 + lens);
    write_le32(p, (uint32_t)lens);
    p[4] = (char)type;
    if (lens > 0) {
        memmove(p + 5, inner ? bson->doc.data + off : val, lens);
    }
}
void bson_append_binary(bson_ctx *bson, const char *key, bson_subtype type, char *val, size_t lens) {
    bson_append_binary2(bson, key, strlen(key), type, val, lens);
}
//signed_byte(7) e_name (byte*12)
void bson_append_oid2(bson_ctx *bson, const char *key, size_t klens, char oid[BSON_OID_LENS]) {
    int32_t inner;
    uintptr_t off = _bson_src_mark(bson, oid, &inner);
    char *p = _bson_append_head(bson, BSON_OID, key, klens, BSON_OID_LENS);
    memmove(p, inner ? bson->doc.data + off : oid, BSON_OID_LENS);
}
void bson_append_oid(bson_ctx *bson, const char *key, char oid[BSON_OID_LENS]) {
    bson_append_oid2(bson, key, strlen(key), oid);
}
//signed_byte(8) e_name unsigned_byte(0/1)
void bson_append_bool2(bson_ctx *bson, const char *key, size_t klens, int8_t b) {
    _bson_append_head(bson, BSON_BOOL, key, klens, 1)[0] = b ? 1 : 0;
}
void bson_append_bool(bson_ctx *bson, const char *key, int8_t b) {
    bson_append_bool2(bson, key, strlen(key), b);
}
//signed_byte(9) e_name int64
void bson_append_date2(bson_ctx *bson, const char *key, size_t klens, int64_t date) {
    write_le64(_bson_append_head(bson, BSON_DATE, key, klens, 8), (uint64_t)date);
}
void bson_append_date(bson_ctx *bson, const char *key, int64_t date) {
    bson_append_date2(bson, key, strlen(key), date);
}
//signed_byte(10) e_name
void bson_append_null2(bson_ctx *bson, const char *key, size_t klens) {
    _bson_append_head(bson, BSON_NULL, key, klens, 0);
}
void bson_append_null(bson_ctx *bson, const char *key) {
    bson_append_null2(bson, key, strlen(key));
}
//signed_byte(11) e_name cstring cstring
void bson_append_regex(bson_ctx *bson, const char *key, const char *pattern, const char *options) {
    int32_t pinner;
    int32_t oinner;
    uintptr_t poff = _bson_src_mark(bson, pattern, &pinner);
    uintptr_t ooff = _bson_src_mark(bson, options, &oinner);
    size_t plens = strlen(pattern);
    size_t olens = strlen(options);
    char *p = _bson_append_head(bson, BSON_REGEX, key, strlen(key), plens + 1 + olens + 1);
    memmove(p, pinner ? bson->doc.data + poff : pattern, plens + 1);
    memmove(p + plens + 1, oinner ? bson->doc.data + ooff : options, olens + 1);
}
void bson_append_jscode_n(bson_ctx *bson, const char *key, const char *jscode, size_t lens) {
    ASSERTAB(lens <= INT32_MAX - 1, "BSON JavaScript code length exceeds 2GB limit");
    _bson_append_str(bson, BSON_JSCODE, key, strlen(key), jscode, lens);
}
//signed_byte(13) e_name string
void bson_append_jscode(bson_ctx *bson, const char *key, const char *jscode) {
    bson_append_jscode_n(bson, key, jscode, strlen(jscode));
}
//signed_byte(16) e_name int32
void bson_append_int322(bson_ctx *bson, const char *key, size_t klens, int32_t val) {
    write_le32(_bson_append_head(bson, BSON_INT32, key, klens, 4), (uint32_t)val);
}
void bson_append_int32(bson_ctx *bson, const char *key, int32_t val) {
    bson_append_int322(bson, key, strlen(key), val);
}
//signed_byte(17) e_name uint64
void bson_append_timestamp(bson_ctx *bson, const char *key, uint32_t ts, uint32_t inc) {
    char *p = _bson_append_head(bson, BSON_TIMESTAMP, key, strlen(key), 8);
    write_le32(p, inc);
    write_le32(p + 4, ts);
}
//signed_byte(18) e_name int64
void bson_append_int642(bson_ctx *bson, const char *key, size_t klens, int64_t val) {
    write_le64(_bson_append_head(bson, BSON_INT64, key, klens, 8), (uint64_t)val);
}
void bson_append_int64(bson_ctx *bson, const char *key, int64_t val) {
    bson_append_int642(bson, key, strlen(key), val);
}
//signed_byte(-1) e_name
void bson_append_minkey(bson_ctx *bson, const char *key) {
    _bson_append_head(bson, BSON_MINKEY, key, strlen(key), 0);
}
//signed_byte(127) e_name
void bson_append_maxkey(bson_ctx *bson, const char *key) {
    _bson_append_head(bson, BSON_MAXKEY, key, strlen(key), 0);
}
// 清空迭代器的当前字段信息（类型、长度、key、val 等）
static inline void _bson_iter_clear(bson_iter *iter) {
    iter->subtype = 0;
    iter->keylens = 0;
    iter->lens = 0;
    iter->key = NULL;
    iter->val = NULL;
    iter->val2 = NULL;
}
// 置"无有效当前元素":type 回 BSON_EOD 哨兵令 _bson_iter_check 对任何真实类型都失败。
// 解析失败路径必须调用,否则 type 停在畸形元素的类型而 val 为 NULL,getter 会通过类型检查后解引用 NULL
static inline void _bson_iter_poison(bson_iter *iter) {
    _bson_iter_clear(iter);
    iter->type = BSON_EOD;
}
void bson_iter_init(bson_iter *iter, bson_ctx *bson) {
    iter->doc = &bson->doc;
    binary_offset(iter->doc, 0);
    size_t lens = 0;
    if (iter->doc->size >= 4) {
        lens = (size_t)binary_get_integer(iter->doc, 4, 1);
    }
    if (lens < 5 || lens > iter->doc->size) {
        iter->doclens = 0;
    } else {
        iter->doclens = lens;
    }
    // doclens 为 0 即长度字段本身非法(< 5 或超出 buffer),属结构错误而非空文档
    iter->err = (0 == iter->doclens) ? 1 : 0;
    _bson_iter_poison(iter);
}
void bson_iter_reset(bson_iter *iter) {
    // doclens 为 0 时 bson_iter_init 已判定长度字段非法,缓冲可能连 4 字节都不到,
    // 再跳过去会撞 binary_offset 的越界断言;非 0 则长度字段已校验过 >= 5,跳 4 必在界内
    if (0 != iter->doclens) {
        binary_offset(iter->doc, 4);
    }
    iter->err = (0 == iter->doclens) ? 1 : 0;
    _bson_iter_poison(iter);
}
int32_t bson_iter_error(const bson_iter *iter) {
    return iter->err;
}
// 从 off 起还能读多少字节;off 越界返 0。注意 lens 传 0 时 "lens > avail" 恒假,越界的 off 会被放行——
// 现有调用点传的都是非零常量,新增调用点须自己确认
static inline size_t _bson_iter_avail(const bson_iter *iter, size_t off) {
    return iter->doclens > off ? iter->doclens - off : 0;
}
// 从 off 起在 doclens 边界内定位一个 NUL 结尾的 C 串;找到返 1 并回填 out/lens
// (不推进 off,两个出参都可传 NULL),找不到返 0。key 与 regex 的两个 cstring 共用这一份边界判定
static inline int32_t _bson_iter_cstring(bson_iter *iter, size_t off, const char **out, uint32_t *lens) {
    const char *start = iter->doc->data + off;
    size_t avail = _bson_iter_avail(iter, off);
    size_t i = 0;
    uint64_t v;
    uint64_t m;
    const char *nul = NULL;
    if (IS_LITTLE) {
        while (i + 8 <= avail) {
            memcpy(&v, start + i, 8);
            // 一次查 8 个字节里有没有 0：每字节减 1，原来是 0 的字节借位变成 0xFF、最高位成 1；
            // 与上 ~v 去掉本来最高位就是 1 的字节。结果非 0 就有 0 字节，最低那个置位字节就是第一个 0
            m = (v - 0x0101010101010101ull) & ~v & 0x8080808080808080ull;
            if (0 != m) {
                nul = start + i + (ctz64(m) >> 3);// 最低置位的下标除以 8 即字节下标
                break;
            }
            i += 8;
        }
    }
    if (NULL == nul) {
        nul = memchr(start + i, '\0', avail - i);
    }
    if (NULL == nul) {
        return 0;
    }
    SET_PTR(out, start);
    SET_PTR(lens, (uint32_t)(nul - start));
    return 1;
}
// 读出 key 并跳过它。返回推进后的 off,失败返 ITER_BAD
static inline size_t _bson_iter_read_key(bson_iter *iter, size_t off) {
    if (0 == _bson_iter_cstring(iter, off, &iter->key, &iter->keylens)) {
        LOG_WARN("invalid bson key.");
        return ITER_BAD;
    }
    return off + iter->keylens + 1;
}
// 定长类型统一读取:read_key + 边界检查 + 取值。返回推进后的 off,失败返 ITER_BAD
static FORCE_INLINE size_t _bson_iter_fixed(bson_iter *iter, size_t off, size_t lens) {
    off = _bson_iter_read_key(iter, off);
    if (ITER_BAD == off) {
        return ITER_BAD;
    }
    if (lens > _bson_iter_avail(iter, off)) {
        LOG_WARN("invalid bson %s.", bson_type_tostring(iter->type));
        return ITER_BAD;
    }
    iter->lens = lens;
    iter->val = iter->doc->data + off;
    return off + lens;
}
// 变长类型统一前导:read_key + 确认还剩 4 字节 + 读 int32 长度 + 校验可读空间。
// 下限与修正量由类型定死,不当参数传:调用处填成数字的话搭错一对照样编过,只是长度校验静默出错;
// 收进来以后新增类型漏写 case 直接落 default 报错。修正量是长度值之外还多占的字节数——
// binary 的 subtype 占 1,document/array 的声明长度含长度字段自身那 4 字节故 -4,
// 字符串的声明长度已含结尾 \0 故为 0。
// 返回长度字段之后的 off(长度字段起点即 off-4),失败返 ITER_BAD
static FORCE_INLINE size_t _bson_iter_lenprefix(bson_iter *iter, size_t off, size_t *out_lens) {
    int64_t min, adjust;
    switch (iter->type) {
    case BSON_UTF8:
    case BSON_JSCODE:
        min = 1;
        adjust = 0;
        break;
    case BSON_DOCUMENT:
    case BSON_ARRAY:
        min = 5;
        adjust = -4;
        break;
    case BSON_BINARY:
        min = 0;
        adjust = 1;
        break;
    default:
        LOG_WARN("bson type %d has no length prefix.", iter->type);
        return ITER_BAD;
    }
    off = _bson_iter_read_key(iter, off);
    if (ITER_BAD == off) {
        return ITER_BAD;
    }
    if (4 > _bson_iter_avail(iter, off)) {
        LOG_WARN("invalid bson %s length.", bson_type_tostring(iter->type));
        return ITER_BAD;
    }
    int64_t lens = (int64_t)(int32_t)read_le32(iter->doc->data + off);
    off += 4;
    if (lens < min
        || (size_t)(lens + adjust) > _bson_iter_avail(iter, off)) {
        LOG_WARN("invalid bson %s length %" PRId64 ".", bson_type_tostring(iter->type), lens);
        return ITER_BAD;
    }
    *out_lens = (size_t)lens;
    return off;
}
// 读一个 cstring 值(regex 的 pattern / options):定位、取指针、跳过它。失败返 ITER_BAD
static inline size_t _bson_iter_cstr_val(bson_iter *iter, size_t off, char **out) {
    const char *start;
    uint32_t rlens;
    if (0 == _bson_iter_cstring(iter, off, &start, &rlens)) {
        LOG_WARN("invalid bson regex.");
        return ITER_BAD;
    }
    *out = (char *)start;
    return off + rlens + 1;
}
int32_t bson_iter_next(bson_iter *iter) {
    size_t off = iter->doc->offset;
    if (off >= iter->doclens) {
        if (BSON_EOD != iter->type) {
            iter->err = 1;
        }
        _bson_iter_poison(iter);
        return 0;
    }
    size_t vlens;
    _bson_iter_clear(iter);
    iter->type = (uint8_t)iter->doc->data[off];//signed_byte(type)
    off++;
    switch (iter->type) {
    case BSON_EOD:
        iter->doc->offset = off;
        _bson_iter_poison(iter);
        return 0;
    case BSON_DOUBLE://e_name double
        off = _bson_iter_fixed(iter, off, sizeof(double));
        break;
    case BSON_UTF8://e_name string
    case BSON_JSCODE://e_name string
        /* 长度字段含末尾 \0，合法值 >= 1；为 0 或负数时 (size_t)(lens-1) 下溢，
         * 读越界。*/
        off = _bson_iter_lenprefix(iter, off, &vlens);
        if (ITER_BAD == off) {
            break;
        }
        iter->lens = vlens - 1;
        iter->val = iter->doc->data + off;
        if ('\0' != iter->val[iter->lens]) {
            off = ITER_BAD;
            LOG_WARN("invalid bson string, not null-terminated.");
            break;
        }
        off += iter->lens + 1;
        break;
    case BSON_DOCUMENT://e_name document
    case BSON_ARRAY://e_name document
        off = _bson_iter_lenprefix(iter, off, &vlens);
        if (ITER_BAD == off) {
            break;
        }
        iter->lens = vlens;
        iter->val = iter->doc->data + off - 4;//子文档要连长度前缀一起带走
        off += iter->lens - 4;
        break;
    case BSON_BINARY://e_name binary
        off = _bson_iter_lenprefix(iter, off, &vlens);
        if (ITER_BAD == off) {
            break;
        }
        iter->lens = vlens;
        iter->subtype = (uint8_t)iter->doc->data[off];//adjust 的 +1 就是这个字节
        off++;
        // 零长 binary 合法,指到当前偏移处即可,有无数据由 lens 表达
        iter->val = iter->doc->data + off;
        off += iter->lens;
        break;
    case BSON_OID://e_name (byte*12)
        off = _bson_iter_fixed(iter, off, BSON_OID_LENS);
        break;
    case BSON_BOOL://e_name unsigned_byte(0/1)
        off = _bson_iter_fixed(iter, off, sizeof(uint8_t));
        break;
    case BSON_DATE://e_name int64
    case BSON_TIMESTAMP://e_name uint64
    case BSON_INT64://e_name int64
        off = _bson_iter_fixed(iter, off, sizeof(uint64_t));
        break;
    case BSON_NULL://e_name
    case BSON_MINKEY://e_name
    case BSON_MAXKEY://e_name
        off = _bson_iter_read_key(iter, off);
        break;
    case BSON_REGEX://e_name cstring(regex pattern) cstring(regex options)
        off = _bson_iter_read_key(iter, off);
        if (ITER_BAD == off) {
            break;
        }
        off = _bson_iter_cstr_val(iter, off, &iter->val);
        if (ITER_BAD == off) {
            break;
        }
        off = _bson_iter_cstr_val(iter, off, &iter->val2);
        break;
    case BSON_INT32://e_name int32
        off = _bson_iter_fixed(iter, off, sizeof(int32_t));
        break;
    case BSON_DECIMAL128://e_name decimal128
        off = _bson_iter_fixed(iter, off, BSON_DECIMAL128_LENS);
        break;
    default:
        off = ITER_BAD;
        LOG_WARN("unsupported bson type %d.", iter->type);
        break;
    }
    // off == doclens 说明这个元素把末尾那字节吃掉了,而合法文档必以 EOD 收尾——
    // 十种元素的边界判定各管各的可读范围,这条"给 EOD 留一字节"的不变式统一在此处判。
    // 上面任一分支失败返的 ITER_BAD 就是 SIZE_MAX,同样落在这条里
    if (off >= iter->doclens) {
        // 元素坏了之后的字节位置就不可信,不再往下猜;offset 推到末尾避免同一个坏元素
        // 被反复重解析(每次重复一条告警),下次进来直接从开头的边界判定返回
        iter->err = 1;
        _bson_iter_poison(iter);
        iter->doc->offset = iter->doclens;
        return 0;
    }
    iter->doc->offset = off;
    return 1;
}
// 在当前层级顺序扫描指定 key;找到时 iter 即停在该元素上
static int32_t _bson_iter_find(bson_iter *iter, const char *key, size_t klens) {
    while (bson_iter_next(iter)) {
        if (klens == (size_t)iter->keylens
            && 0 == memcmp(iter->key, key, klens)) {
            return ERR_OK;
        }
    }
    return ERR_FAILED;
}
int32_t bson_iter_find(bson_iter *iter, const char *keys, bson_iter *result) {
    const char *seg = keys;
    const char *kend = keys + strlen(keys);
    const char *dot;
    size_t slens;
    int32_t depth = 0;
    int32_t rtn = ERR_FAILED;
    // 扫描全程走独立游标,不碰调用方的 doc:没找到时 iter 要保持原位,而 result 可与 iter 同体
    bson_iter cur = *iter;
    bson_ctx sub;
    cur.nested_doc = *iter->doc;
    cur.doc = &cur.nested_doc;
    for (;;) {
        depth++;
        if (depth > BSON_MAX_DEPTH) {
            LOG_WARN("bson key path exceeds %d segments.", BSON_MAX_DEPTH);
            rtn = ERR_FAILED;
            break;
        }
        dot = memchr(seg, '.', (size_t)(kend - seg));
        slens = (NULL == dot) ? (size_t)(kend - seg) : (size_t)(dot - seg);
        // 空段一律非法,只有"整个 keys 就是一个空键名"例外——空串是合法的 BSON 键名
        if (0 == slens
            && !(seg == keys && NULL == dot)) {
            rtn = ERR_FAILED;
            break;
        }
        rtn = _bson_iter_find(&cur, seg, slens);
        if (ERR_OK != rtn
            || NULL == dot) {
            break;
        }
        if (BSON_DOCUMENT != cur.type
            && BSON_ARRAY != cur.type) {
            rtn = ERR_FAILED;
            break;
        }
        bson_init(&sub, cur.val, cur.lens);
        bson_iter_init(&cur, &sub);
        seg = dot + 1;
    }
    if (ERR_OK != rtn) {
        // "文档结构非法"是文档级事实要留住:调用方靠 bson_iter_error 分辨"没这个 key"
        // 和"后面全坏了"。只置不清,别把进函数前就有的标志抹掉
        if (0 != cur.err) {
            iter->err = 1;
        }
        return rtn;
    }
    // 末层视图必须拷进 result 自持:点分路径下 cur.doc 指向本函数栈上的 sub,带出去就是死地址
    cur.nested_doc = *cur.doc;
    *result = cur;
    result->doc = &result->nested_doc;
    return ERR_OK;
}
// 检查迭代器当前字段类型是否与期望类型一致，不一致时设置 err
static int32_t _bson_iter_check(bson_iter *iter, bson_type type, int32_t *err) {
    if (type != iter->type) {
        SET_PTR(err, ERR_FAILED);
        return ERR_FAILED;
    }
    SET_PTR(err, ERR_OK);
    return ERR_OK;
}
double bson_iter_double(bson_iter *iter, int32_t *err) {
    if (ERR_OK != _bson_iter_check(iter, BSON_DOUBLE, err)) {
        return 0;
    }
    return unpack_double(iter->val, 1);
}
const char *bson_iter_utf8(bson_iter *iter, int32_t *err) {
    if (ERR_OK != _bson_iter_check(iter, BSON_UTF8, err)) {
        return NULL;
    }
    return iter->val;
}
char *bson_iter_document(bson_iter *iter, size_t *lens, int32_t *err) {
    if (ERR_OK != _bson_iter_check(iter, BSON_DOCUMENT, err)) {
        return NULL;
    }
    *lens = iter->lens;
    return iter->val;
}
char *bson_iter_array(bson_iter *iter, size_t *lens, int32_t *err) {
    if (ERR_OK != _bson_iter_check(iter, BSON_ARRAY, err)) {
        return NULL;
    }
    *lens = iter->lens;
    return iter->val;
}
char *bson_iter_binary(bson_iter *iter, bson_subtype *subtype, size_t *lens, int32_t *err) {
    if (ERR_OK != _bson_iter_check(iter, BSON_BINARY, err)) {
        return NULL;
    }
    SET_PTR(subtype, iter->subtype);
    *lens = iter->lens;
    return iter->val;
}
char *bson_iter_oid(bson_iter *iter, int32_t *err) {
    if (ERR_OK != _bson_iter_check(iter, BSON_OID, err)) {
        return NULL;
    }
    return iter->val;
}
int32_t bson_iter_bool(bson_iter *iter, int32_t *err) {
    if (ERR_OK != _bson_iter_check(iter, BSON_BOOL, err)) {
        return 0;
    }
    return iter->val[0];
}
int64_t bson_iter_date(bson_iter *iter, int32_t *err) {
    if (ERR_OK != _bson_iter_check(iter, BSON_DATE, err)) {
        return 0;
    }
    return (int64_t)read_le64(iter->val);
}
const char *bson_iter_regex(bson_iter *iter, char **options, int32_t *err) {
    if (ERR_OK != _bson_iter_check(iter, BSON_REGEX, err)) {
        return NULL;
    }
    SET_PTR(options, iter->val2);
    return iter->val;
}
const char *bson_iter_jscode(bson_iter *iter, int32_t *err) {
    if (ERR_OK != _bson_iter_check(iter, BSON_JSCODE, err)) {
        return NULL;
    }
    return iter->val;
}
int32_t bson_iter_int32(bson_iter *iter, int32_t *err) {
    if (ERR_OK != _bson_iter_check(iter, BSON_INT32, err)) {
        return 0;
    }
    return (int32_t)read_le32(iter->val);
}
uint32_t bson_iter_timestamp(bson_iter *iter, uint32_t *inc, int32_t *err) {
    if (ERR_OK != _bson_iter_check(iter, BSON_TIMESTAMP, err)) {
        return 0;
    }
    *inc = read_le32(iter->val);
    return read_le32(iter->val + 4);
}
int64_t bson_iter_int64(bson_iter *iter, int32_t *err) {
    if (ERR_OK != _bson_iter_check(iter, BSON_INT64, err)) {
        return 0;
    }
    return (int64_t)read_le64(iter->val);
}
const char *bson_type_tostring(bson_type type) {
    switch (type) {
    case BSON_DOUBLE:
        return "double";
    case BSON_UTF8:
        return "string";
    case BSON_DOCUMENT:
        return "object";
    case BSON_ARRAY:
        return "array";
    case BSON_BINARY:
        return "binData";
    case BSON_OID:
        return "objectId";
    case BSON_BOOL:
        return "bool";
    case BSON_DATE:
        return "date";
    case BSON_NULL:
        return "null";
    case BSON_REGEX:
        return "regex";
    case BSON_JSCODE:
        return "javascript";
    case BSON_INT32:
        return "int";
    case BSON_TIMESTAMP:
        return "timestamp";
    case BSON_INT64:
        return "long";
    case BSON_DECIMAL128:
        return "decimal";
    case BSON_MINKEY:
        return "minKey";
    case BSON_MAXKEY:
        return "maxKey";
    default:
        return "unknown";
    }
}
const char *bson_subtype_tostring(bson_subtype type) {
    switch (type) {
    case BSON_SUBTYPE_BINARY:
        return "binary";
    case BSON_SUBTYPE_FUNCTION:
        return "function";
    case BSON_SUBTYPE_UUID:
        return "uuid";
    case BSON_SUBTYPE_MD5:
        return "md5";
    case BSON_SUBTYPE_ENCRYPTED:
        return "encrypted";
    case BSON_SUBTYPE_COMPRESSED:
        return "compressed";
    case BSON_SUBTYPE_SENSITIVE:
        return "sensitive";
    case BSON_SUBTYPE_VECTOR:
        return "vector";
    case BSON_SUBTYPE_USER:
        return "user";
    default:
        return "unknown";
    }
}
// 递归检查嵌套深度，自身受 BSON_MAX_DEPTH 限制，单帧栈消耗有界
static int32_t _bson_check_depth(char *data, size_t lens, int32_t depth) {
    if (depth > BSON_MAX_DEPTH) {
        return ERR_FAILED;
    }
    bson_ctx sub;
    bson_iter iter;
    bson_init(&sub, data, lens);
    bson_iter_init(&iter, &sub);
    while (bson_iter_next(&iter)) {
        // 直接用 iter.val / iter.lens: 类型已在条件里判过, bson_iter_document / _array
        // 只是"校验 type + 回填 lens + 返回 val", 那道校验必过
        if (BSON_DOCUMENT == iter.type
            || BSON_ARRAY == iter.type) {
            if (ERR_OK != _bson_check_depth(iter.val, iter.lens, depth + 1)) {
                return ERR_FAILED;
            }
        }
    }
    // 循环退出有两种可能:读到 EOD 正常结束,或某个元素非法被拒。后者只检查了坏元素之前的前缀
    if (0 != bson_iter_error(&iter)) {
        return ERR_FAILED;
    }
    // EOD 提前出现时 err 不置位(读到的确实是合法的 EOD),但声明长度还剩字节没用掉。
    // 读完 EOD 后 offset 恰好推过它,合法文档必然等于 doclens
    if (iter.doc->offset != iter.doclens) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t bson_check_depth(char *data, size_t lens) {
    if (NULL == data) {
        return ERR_FAILED;
    }
    return _bson_check_depth(data, lens, 0);
}
// 按已知长度写入串化文本，内嵌 NUL 转义成可见的 "\\0"。
// BSON 字符串允许内嵌 NUL（写入侧 bson_append_utf8_n 就是长度感知的），而串化结果的两个
// 消费者都按 NUL 结尾读(如 mongo_parse 的 LOG_WARN("%s", ...))——
// 原样写进去后半段谁也看不到，等于让运维只拿到半截错误
static void _bson_dump_text(binary_ctx *str, const char *val, size_t lens) {
    size_t beg = 0;
    const char *nul;
    while (NULL != (nul = memchr(val + beg, '\0', lens - beg))) {
        binary_set_binary(str, val + beg, (size_t)(nul - val) - beg);
        binary_set_binary(str, "\\0", 2);
        beg = (size_t)(nul - val) + 1;
    }
    binary_set_binary(str, val + beg, lens - beg);
}
// 递归将 BSON 文档格式化为带缩进的可读字符串，追加到 str 中
// depth 用于限制递归深度，超过 BSON_MAX_DEPTH 时截断，防止恶意深嵌套触发栈溢出
static void _bson_dump(bson_ctx *bson, int32_t index, int32_t depth, binary_ctx *str) {
    if (depth > BSON_MAX_DEPTH) {
        binary_set_fill(str, ' ', index * 4);
        binary_set_binary(str, "...(too deep)\r\n", 15);
        return;
    }
    bson_iter iter;
    bson_iter_init(&iter, bson);
    const char *strtype;
    bson_ctx child;
    size_t lens;
    bson_subtype subtype;
    const char *subtstr;
    uint32_t inc;
    uint32_t ts;
    char *options;
    // 各 case 的取值变量按类型分开命名、一并提到循环外:叫同一个名字就得给每个
    // case 套一层花括号(房屋规范禁止裸花括号只为限定作用域)
    double dval;
    int32_t ival;
    int64_t i64val;
    const char *cstr;
    char *bin;
    while (bson_iter_next(&iter)) {
        binary_set_fill(str, ' ', index * 4);
        binary_set_binary(str, iter.key, iter.keylens);
        binary_set_binary(str, "(", 1);
        strtype = bson_type_tostring(iter.type);
        binary_set_binary(str, strtype, strlen(strtype));
        binary_set_binary(str, "): ", 3);
        switch (iter.type) {
        case BSON_DOUBLE:
            dval = bson_iter_double(&iter, NULL);
            binary_set_va(str, "%lf", dval);
            break;
        case BSON_UTF8:
            cstr = bson_iter_utf8(&iter, NULL);
            _bson_dump_text(str, cstr, iter.lens);
            break;
        case BSON_JSCODE:
            cstr = bson_iter_jscode(&iter, NULL);
            _bson_dump_text(str, cstr, iter.lens);
            break;
        case BSON_DOCUMENT:
        case BSON_ARRAY:
            bson_init(&child, iter.val, iter.lens);
            if (BSON_DOCUMENT == iter.type) {
                binary_set_binary(str, "{\r\n", 3);
            } else {
                binary_set_binary(str, "[\r\n", 3);
            }
            _bson_dump(&child, index + 1, depth + 1, str);
            binary_set_fill(str, ' ', index * 4);
            if (BSON_DOCUMENT == iter.type) {
                binary_set_binary(str, "}", 1);
            } else {
                binary_set_binary(str, "]", 1);
            }
            break;
        case BSON_BINARY:
            bin = bson_iter_binary(&iter, &subtype, &lens, NULL);
            subtstr = bson_subtype_tostring(subtype);
            binary_set_binary(str, "(", 1);
            binary_set_binary(str, subtstr, strlen(subtstr));
            binary_set_binary(str, ") ", 2);
            binary_set_skip(str, lens * 2);
            tohex(bin, lens, str->data + str->offset - lens * 2, 0);
            break;
        case BSON_OID:
            bin = bson_iter_oid(&iter, NULL);
            binary_set_skip(str, BSON_OID_LENS * 2);
            tohex(bin, BSON_OID_LENS, str->data + str->offset - BSON_OID_LENS * 2, 0);
            break;
        case BSON_BOOL:
            ival = bson_iter_bool(&iter, NULL);
            if (ival) {
                binary_set_binary(str, "true", strlen("true"));
            } else {
                binary_set_binary(str, "false", strlen("false"));
            }
            break;
        case BSON_TIMESTAMP:
            inc = 0;
            ts = bson_iter_timestamp(&iter, &inc, NULL);
            binary_set_va(str, "%u %u", inc, ts);
            break;
        case BSON_DATE:
            i64val = bson_iter_date(&iter, NULL);
            binary_set_va(str, "%"PRId64, i64val);
            break;
        case BSON_INT64:
            i64val = bson_iter_int64(&iter, NULL);
            binary_set_va(str, "%"PRId64, i64val);
            break;
        case BSON_DECIMAL128:
            binary_set_skip(str, BSON_DECIMAL128_LENS * 2);
            tohex(iter.val, BSON_DECIMAL128_LENS, str->data + str->offset - BSON_DECIMAL128_LENS * 2, 0);
            break;
        case BSON_NULL:
        case BSON_MINKEY:
        case BSON_MAXKEY:
            break;
        case BSON_REGEX:
            cstr = bson_iter_regex(&iter, &options, NULL);
            binary_set_binary(str, cstr, strlen(cstr));
            binary_set_fill(str, ' ', 4);
            binary_set_binary(str, options, strlen(options));
            break;
        case BSON_INT32:
            ival = (int32_t)bson_iter_int32(&iter, NULL);
            binary_set_va(str, "%d", ival);
            break;
        default:
            break;
        }
        binary_set_binary(str, FLAG_CRLF, CRLF_SIZE);
    }
}
char *bson_tostring(bson_ctx *bson) {
    size_t offset = bson->doc.offset;
    binary_offset(&bson->doc, 0);
    binary_ctx str;
    binary_init_write(&str, 0, 0);
    binary_set_binary(&str, "{\r\n", 3);
    _bson_dump(bson, 1, 0, &str);
    binary_set_binary(&str, "}", 1);
    binary_set_int8(&str, 0);
    binary_offset(&bson->doc, offset);
    return str.data;
}
char *bson_tostring2(char *data, size_t lens) {
    if (NULL == data) {
        return NULL;
    }
    bson_ctx bson;
    bson_init(&bson, data, lens);
    return bson_tostring(&bson);
}
