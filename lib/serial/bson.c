#include "serial/bson.h"

// bson_cat / 迭代器 / bson_check_depth 三处校验严在不同的轴上, 别拿一处的宽松当漏检去补齐。
// 最易踩: 提前出现 EOD 后还剩没用掉的字节, 迭代器当遍历正常结束静默忽略, 只有 bson_check_depth 拒

#define BSON_APPEND_CSTRING(str) binary_set_string(&bson->doc, str)
#define BSON_APPEND_KEY(type) \
    binary_set_int8(&bson->doc, (int8_t)type);\
    BSON_APPEND_CSTRING(key)

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
    pack_integer((char *)_oid_header + 3, (uint64_t)(uint32_t)pid, 2, 1);
    _oid_counter = randrange(10000, 20000);
}
// 初始化静态空 BSON 文档（长度为5：4字节长度 + 1字节 EOD）
static void _bson_empty_init(void) {
    pack_integer(_bson_empty, 5, 4, 1);
}
void bson_globle_init(void) {
    _bson_oid_init();
    _bson_empty_init();
}
void bson_oid(char oid[BSON_OID_LENS]) {
    time_t ti = time(NULL);
    uint32_t id = ATOMIC_ADD(&_oid_counter, 1);
    pack_integer(oid, (uint64_t)ti, 4, 0);
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
void bson_init(bson_ctx *bson, char *data, size_t lens) {
    bson->depth = 0;
    binary_init(&bson->doc, data, lens, 0);
    if (NULL == data) {
        _bson_append_start(bson);
    }
}
int32_t bson_complete(bson_ctx *bson) {
    return 0 == bson->depth && bson->doc.offset > 0;
}
void bson_append_end(bson_ctx *bson) {
    ASSERTAB(bson->depth > 0, "logic error.");
    binary_set_int8(&bson->doc, BSON_EOD);
    size_t endoff = bson->doc.offset;
    size_t startoff = bson->offsets[bson->depth - 1];
    binary_offset(&bson->doc, startoff);
    binary_set_integer(&bson->doc, endoff - startoff, 4, 1);
    binary_offset(&bson->doc, endoff);
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
    uint32_t doclens = (uint32_t)unpack_integer(doc, 4, 1, 0);
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
void bson_append_document_begain(bson_ctx *bson, const char *key) {
    BSON_APPEND_KEY(BSON_DOCUMENT);
    _bson_append_start(bson);
}
void bson_append_array_begain(bson_ctx *bson, const char *key) {
    BSON_APPEND_KEY(BSON_ARRAY);
    _bson_append_start(bson);
}
//signed_byte(1) e_name double
void bson_append_double(bson_ctx *bson, const char *key, double val) {
    BSON_APPEND_KEY(BSON_DOUBLE);
    binary_set_double(&bson->doc, val, 1);
}
//signed_byte(2) e_name string
void bson_append_utf8_n(bson_ctx *bson, const char *key, const char *val, size_t lens) {
    ASSERTAB(lens <= INT32_MAX - 1, "BSON UTF-8 string length exceeds 2GB limit");
    BSON_APPEND_KEY(BSON_UTF8);
    binary_set_integer(&bson->doc, lens + 1, 4, 1);
    binary_set_binary(&bson->doc, val, lens);
    binary_set_int8(&bson->doc, 0);
}
void bson_append_utf8(bson_ctx *bson, const char *key, const char *val) {
    bson_append_utf8_n(bson, key, val, strlen(val));
}
//signed_byte(3) e_name document
void bson_append_document(bson_ctx *bson, const char *key, char *doc, size_t lens) {
    BSON_APPEND_KEY(BSON_DOCUMENT);
    binary_set_binary(&bson->doc, doc, lens);
}
//signed_byte(4) e_name document
void bson_append_array(bson_ctx *bson, const char *key, char *doc, size_t lens) {
    BSON_APPEND_KEY(BSON_ARRAY);
    binary_set_binary(&bson->doc, doc, lens);
}
//signed_byte(5) e_name binary
void bson_append_binary(bson_ctx *bson, const char *key, bson_subtype type, char *val, size_t lens) {
    ASSERTAB(lens <= INT32_MAX, "BSON binary length exceeds 2GB limit");
    BSON_APPEND_KEY(BSON_BINARY);
    binary_set_integer(&bson->doc, lens, 4, 1);
    binary_set_int8(&bson->doc, type);
    binary_set_binary(&bson->doc, val, lens);
}
//signed_byte(7) e_name (byte*12)
void bson_append_oid(bson_ctx *bson, const char *key, char oid[BSON_OID_LENS]) {
    BSON_APPEND_KEY(BSON_OID);
    binary_set_binary(&bson->doc, oid, BSON_OID_LENS);
}
//signed_byte(8) e_name unsigned_byte(0/1)
void bson_append_bool(bson_ctx *bson, const char *key, int8_t b) {
    BSON_APPEND_KEY(BSON_BOOL);
    binary_set_int8(&bson->doc, b ? 1 : 0);
}
//signed_byte(9) e_name int64
void bson_append_date(bson_ctx *bson, const char *key, int64_t date) {
    BSON_APPEND_KEY(BSON_DATE);
    binary_set_integer(&bson->doc, date, 8, 1);
}
//signed_byte(10) e_name
void bson_append_null(bson_ctx *bson, const char *key) {
    BSON_APPEND_KEY(BSON_NULL);
}
//signed_byte(11) e_name cstring cstring
void bson_append_regex(bson_ctx *bson, const char *key, const char *pattern, const char *options) {
    BSON_APPEND_KEY(BSON_REGEX);
    BSON_APPEND_CSTRING(pattern);
    BSON_APPEND_CSTRING(options);
}
void bson_append_jscode_n(bson_ctx *bson, const char *key, const char *jscode, size_t lens) {
    ASSERTAB(lens <= INT32_MAX - 1, "BSON JavaScript code length exceeds 2GB limit");
    BSON_APPEND_KEY(BSON_JSCODE);
    binary_set_integer(&bson->doc, lens + 1, 4, 1);
    binary_set_binary(&bson->doc, jscode, lens);
    binary_set_int8(&bson->doc, 0);
}
//signed_byte(13) e_name string
void bson_append_jscode(bson_ctx *bson, const char *key, const char *jscode) {
    bson_append_jscode_n(bson, key, jscode, strlen(jscode));
}
//signed_byte(16) e_name int32
void bson_append_int32(bson_ctx *bson, const char *key, int32_t val) {
    BSON_APPEND_KEY(BSON_INT32);
    binary_set_integer(&bson->doc, val, 4, 1);
}
//signed_byte(17) e_name uint64
void bson_append_timestamp(bson_ctx *bson, const char *key, uint32_t ts, uint32_t inc) {
    BSON_APPEND_KEY(BSON_TIMESTAMP);
    binary_set_integer(&bson->doc, inc, 4, 1);
    binary_set_integer(&bson->doc, ts, 4, 1);
}
//signed_byte(18) e_name int64
void bson_append_int64(bson_ctx *bson, const char *key, int64_t val) {
    BSON_APPEND_KEY(BSON_INT64);
    binary_set_integer(&bson->doc, val, 8, 1);
}
//signed_byte(-1) e_name
void bson_append_minkey(bson_ctx *bson, const char *key) {
    BSON_APPEND_KEY(BSON_MINKEY);
}
//signed_byte(127) e_name
void bson_append_maxkey(bson_ctx *bson, const char *key) {
    BSON_APPEND_KEY(BSON_MAXKEY);
}
// 清空迭代器的当前字段信息（类型、长度、key、val 等）
static void _bson_iter_clear(bson_iter *iter) {
    iter->subtype = 0;
    iter->keylens = 0;
    iter->lens = 0;
    iter->key = NULL;
    iter->val = NULL;
    iter->val2 = NULL;
}
// 置"无有效当前元素":type 回 BSON_EOD 哨兵令 _bson_iter_check 对任何真实类型都失败。
// 解析失败路径必须调用,否则 type 停在畸形元素的类型而 val 为 NULL,getter 会通过类型检查后解引用 NULL
static void _bson_iter_poison(bson_iter *iter) {
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
// 从当前 offset 起在 doclens 边界内定位一个 NUL 结尾的 C 串;找到返 1 并回填 out/lens
// (不推进 offset,两个出参都可传 NULL),找不到返 0。key 与 regex 的两个 cstring 共用这一份边界判定
static int32_t _bson_iter_cstring(bson_iter *iter, const char **out, uint32_t *lens) {
    const char *start = iter->doc->data + iter->doc->offset;
    size_t avail = iter->doclens > iter->doc->offset ? iter->doclens - iter->doc->offset : 0;
    const char *nul = memchr(start, '\0', avail);
    if (NULL == nul) {
        return 0;
    }
    SET_PTR(out, start);
    SET_PTR(lens, (uint32_t)(nul - start));
    return 1;
}
static int32_t _bson_iter_read_key(bson_iter *iter) {
    if (0 == _bson_iter_cstring(iter, &iter->key, &iter->keylens)) {
        LOG_WARN("invalid bson key.");
        return 0;
    }
    binary_offset(iter->doc, iter->doc->offset + iter->keylens + 1);
    return 1;
}
// 定长类型统一读取:read_key + 边界检查 + binary_get_binary;成功返 ERR_OK,失败返 ERR_FAILED
static int32_t _bson_iter_fixed(bson_iter *iter, size_t lens) {
    if (0 == _bson_iter_read_key(iter)) {
        return ERR_FAILED;
    }
    iter->lens = lens;
    if (iter->doc->offset > iter->doclens
        || iter->lens > iter->doclens - iter->doc->offset) {
        LOG_WARN("invalid bson %s.", bson_type_tostring(iter->type));
        return ERR_FAILED;
    }
    iter->val = binary_get_binary(iter->doc, iter->lens);
    return ERR_OK;
}
// 变长类型统一前导:read_key + 确认还剩 4 字节 + 读 int32 长度 + 校验可读空间;成功返 ERR_OK。
// 下限与修正量由类型定死,不当参数传:调用处填成数字的话搭错一对照样编过,只是长度校验静默出错;
// 收进来以后新增类型漏写 case 直接落 default 报错。修正量是长度值之外还要占的字节数——
// 字符串的结尾 \0 与 binary 的 subtype 各 +1,document/array 的声明长度把长度字段自身那 4 字节
// 也算进去了故 -4。out_off 为长度字段起点,document/array 靠它回退把长度前缀一起带走,不需要可传 NULL
static int32_t _bson_iter_lenprefix(bson_iter *iter, size_t *out_lens, size_t *out_off) {
    int64_t min, adjust;
    switch (iter->type) {
    case BSON_UTF8:
    case BSON_JSCODE:
        min = 1;
        adjust = 1;
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
        return ERR_FAILED;
    }
    if (0 == _bson_iter_read_key(iter)) {
        return ERR_FAILED;
    }
    if (iter->doc->offset > iter->doclens
        || 4 > iter->doclens - iter->doc->offset) {
        LOG_WARN("invalid bson %s length.", bson_type_tostring(iter->type));
        return ERR_FAILED;
    }
    size_t off = iter->doc->offset;
    int64_t lens = binary_get_integer(iter->doc, 4, 1);
    if (lens < min
        || iter->doc->offset > iter->doclens
        || (size_t)(lens + adjust) > iter->doclens - iter->doc->offset) {
        LOG_WARN("invalid bson %s length %" PRId64 ".", bson_type_tostring(iter->type), lens);
        return ERR_FAILED;
    }
    *out_lens = (size_t)lens;
    SET_PTR(out_off, off);
    return ERR_OK;
}
int32_t bson_iter_next(bson_iter *iter) {
    if (iter->doc->offset >= iter->doclens) {
        if (BSON_EOD != iter->type) {
            iter->err = 1;
        }
        _bson_iter_poison(iter);
        return 0;
    }
    size_t off;
    size_t vlens;
    int32_t more = 1;
    _bson_iter_clear(iter);
    iter->type = (uint8_t)binary_get_int8(iter->doc);//signed_byte(type)
    switch (iter->type) {
    case BSON_EOD:
        more = 0;
        break;
    case BSON_DOUBLE://e_name double
        if (ERR_OK != _bson_iter_fixed(iter, sizeof(double))) {
            more = 0;
        }
        break;
    case BSON_UTF8://e_name string
    case BSON_JSCODE://e_name string
        /* 长度字段含末尾 \0，合法值 >= 1；为 0 或负数时 (size_t)(lens-1) 下溢，
         * binary_get_binary 读越界。*/
        if (ERR_OK != _bson_iter_lenprefix(iter, &vlens, NULL)) {
            more = 0;
            break;
        }
        iter->lens = vlens - 1;
        iter->val = binary_get_binary(iter->doc, iter->lens + 1);
        if ('\0' != iter->val[iter->lens]) {
            iter->val = NULL;
            more = 0;
            LOG_WARN("invalid bson string, not null-terminated.");
        }
        break;
    case BSON_DOCUMENT://e_name document
    case BSON_ARRAY://e_name document
        if (ERR_OK != _bson_iter_lenprefix(iter, &vlens, &off)) {
            more = 0;
            break;
        }
        iter->lens = vlens;
        binary_offset(iter->doc, off);//回退到长度字段起点，子文档要连长度前缀一起带走
        iter->val = binary_get_binary(iter->doc, iter->lens);
        break;
    case BSON_BINARY://e_name binary
        if (ERR_OK != _bson_iter_lenprefix(iter, &vlens, NULL)) {
            more = 0;
            break;
        }
        iter->lens = vlens;
        iter->subtype = (uint8_t)binary_get_int8(iter->doc);//adjust 的 +1 就是这个字节
        // 零长 binary 合法,但 binary_get_binary 对 lens==0 返 NULL,会破坏"type 有效 ⟹ val 非 NULL";
        // 指到当前偏移处,有无数据由 lens 表达
        iter->val = (0 == iter->lens)
            ? iter->doc->data + iter->doc->offset
            : binary_get_binary(iter->doc, iter->lens);
        break;
    case BSON_OID://e_name (byte*12)
        if (ERR_OK != _bson_iter_fixed(iter, BSON_OID_LENS)) {
            more = 0;
        }
        break;
    case BSON_BOOL://e_name unsigned_byte(0/1)
        if (ERR_OK != _bson_iter_fixed(iter, sizeof(uint8_t))) {
            more = 0;
        }
        break;
    case BSON_DATE://e_name int64
    case BSON_TIMESTAMP://e_name uint64
    case BSON_INT64://e_name int64
        if (ERR_OK != _bson_iter_fixed(iter, sizeof(uint64_t))) {
            more = 0;
        }
        break;
    case BSON_NULL://e_name
    case BSON_MINKEY://e_name
    case BSON_MAXKEY://e_name
        if (0 == _bson_iter_read_key(iter)) {
            more = 0;
        }
        break;
    case BSON_REGEX://e_name cstring(regex pattern) cstring(regex options)
        if (0 == _bson_iter_read_key(iter)) {
            more = 0;
            break;
        }
        if (0 == _bson_iter_cstring(iter, NULL, NULL)) {
            more = 0;
            LOG_WARN("invalid bson regex.");
            break;
        }
        iter->val = binary_get_string(iter->doc);
        if (0 == _bson_iter_cstring(iter, NULL, NULL)) {
            more = 0;
            LOG_WARN("invalid bson regex.");
            break;
        }
        iter->val2 = binary_get_string(iter->doc);
        break;
    case BSON_INT32://e_name int32
        if (ERR_OK != _bson_iter_fixed(iter, sizeof(int32_t))) {
            more = 0;
        }
        break;
    case BSON_DECIMAL128://e_name decimal128
        if (ERR_OK != _bson_iter_fixed(iter, BSON_DECIMAL128_LENS)) {
            more = 0;
        }
        break;
    default:
        more = 0;
        LOG_WARN("unsupported bson type %d.", iter->type);
        break;
    }
    if (0 == more) {
        // switch 里唯一合法的 more=0 是 case BSON_EOD;其余都是解析失败。
        // 必须在毒化之前判断——_bson_iter_poison 会把 type 置成 BSON_EOD
        if (BSON_EOD != iter->type) {
            iter->err = 1;
        }
        _bson_iter_poison(iter);
    }
    return more;
}
// 在当前层级查找指定 key，找到时将 result 设为当前 iter
static int32_t _bson_iter_find(bson_iter *iter, const char *key, size_t klens, bson_iter *result) {
    while (bson_iter_next(iter)) {
        if (klens == (size_t)iter->keylens
            && 0 == memcmp(iter->key, key, klens)) {
            *result = *iter;
            return ERR_OK;
        }
    }
    return ERR_FAILED;
}
int32_t bson_iter_find(bson_iter *iter, const char *keys, bson_iter *result) {
    int32_t rtn = ERR_FAILED;
    size_t klens = strlen(keys);
    // result 可与 iter 是同一对象,下面会把 iter->doc 改指到 nested_doc,还原得按原文档来
    binary_ctx *doc = iter->doc;
    size_t offset = doc->offset;
    if (NULL == strstr(keys, ".")) {
        rtn = _bson_iter_find(iter, keys, klens, result);
        if (ERR_OK != rtn) {
            binary_offset(doc, offset);
            return rtn;
        }
        // 先存下"已推进到该元素之后"的视图再还原: doc 可能就是 result 自己的 nested_doc
        // (链式原地收窄),那时先还原就把这个视图一起倒回去了,next 会把该元素重吐一遍
        binary_ctx cur = *doc;
        binary_offset(doc, offset);
        result->nested_doc = cur;
        result->doc = &result->nested_doc;
        return ERR_OK;
    }
    buf_ctx segs[BSON_MAX_DEPTH];
    buf_ctx *psegs = segs;
    int32_t n = split((char *)keys, klens, ".", 1, &psegs, BSON_MAX_DEPTH, 0);
    if (n < 0) {
        binary_offset(doc, offset);
        return ERR_FAILED;
    }
    bson_iter cur_iter = *iter;
    bson_iter found;
    bson_ctx bson;
    for (int32_t i = 0; i < n; i++) {
        if (0 == segs[i].lens) {
            rtn = ERR_FAILED;
            break;
        }
        rtn = _bson_iter_find(&cur_iter, segs[i].data, segs[i].lens, &found);
        if (ERR_OK != rtn) {
            break;
        }
        if (i == n - 1) {//最后一层
            break;
        }
        if (BSON_DOCUMENT != found.type
            && BSON_ARRAY != found.type) {
            rtn = ERR_FAILED;
            break;
        }
        bson_init(&bson, found.val, found.lens);
        bson_iter_init(&cur_iter, &bson);
    }
    // 还原排在装 result 之前: doc 可能就是 result 自己的 nested_doc(链式原地收窄),
    // 后还原就把刚装好的子文档视图按外层偏移改了, 越界即撞 binary_offset 的断言
    binary_offset(doc, offset);
    if (ERR_OK == rtn) {
        // n >= 2(有点号才走到这里),成功即至少跑过一次非末层分支,bson 必已初始化
        *result = found;
        result->nested_doc = bson.doc;
        result->doc = &result->nested_doc;
    }
    return rtn;
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
    return unpack_integer(iter->val, (int32_t)iter->lens, 1, 0);
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
    return (int32_t)unpack_integer(iter->val, (int32_t)iter->lens, 1, 1);
}
uint32_t bson_iter_timestamp(bson_iter *iter, uint32_t *inc, int32_t *err) {
    if (ERR_OK != _bson_iter_check(iter, BSON_TIMESTAMP, err)) {
        return 0;
    }
    *inc = (uint32_t)unpack_integer(iter->val, 4, 1, 1);
    return (uint32_t)unpack_integer(iter->val + 4, 4, 1, 1);
}
int64_t bson_iter_int64(bson_iter *iter, int32_t *err) {
    if (ERR_OK != _bson_iter_check(iter, BSON_INT64, err)) {
        return 0;
    }
    return unpack_integer(iter->val, (int32_t)iter->lens, 1, 1);
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
    for (size_t i = 0; i < lens; i++) {
        if ('\0' != val[i]) {
            continue;
        }
        binary_set_binary(str, val + beg, i - beg);
        binary_set_binary(str, "\\0", 2);
        beg = i + 1;
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
    while (bson_iter_next(&iter)) {
        binary_set_fill(str, ' ', index * 4);
        binary_set_binary(str, iter.key, iter.keylens);
        binary_set_binary(str, "(", 1);
        strtype = bson_type_tostring(iter.type);
        binary_set_binary(str, strtype, strlen(strtype));
        binary_set_binary(str, ")", 1);
        binary_set_binary(str, ": ", 2);
        switch (iter.type) {
        case BSON_DOUBLE: {
            double val = bson_iter_double(&iter, NULL);
            binary_set_va(str, "%lf", val);
            break;
        }
        case BSON_UTF8: {
            const char *val = bson_iter_utf8(&iter, NULL);
            _bson_dump_text(str, val, iter.lens);
            break;
        }
        case BSON_JSCODE: {
            const char *val = bson_iter_jscode(&iter, NULL);
            _bson_dump_text(str, val, iter.lens);
            break;
        }
        case BSON_DOCUMENT:
        case BSON_ARRAY: {
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
        }
        case BSON_BINARY: {
            char *val = bson_iter_binary(&iter, &subtype, &lens, NULL);
            subtstr = bson_subtype_tostring(subtype);
            binary_set_binary(str, "(", 1);
            binary_set_binary(str, subtstr, strlen(subtstr));
            binary_set_binary(str, ") ", 2);
            char *hex;
            MALLOC(hex, HEX_ENSIZE(lens));
            tohex(val, lens, hex, 0);
            binary_set_binary(str, hex, strlen(hex));
            FREE(hex);
            break;
        }
        case BSON_OID: {
            char *val = bson_iter_oid(&iter, NULL);
            char hex[HEX_ENSIZE(BSON_OID_LENS)];
            tohex(val, BSON_OID_LENS, hex, 0);
            binary_set_binary(str, hex, strlen(hex));
            break;
        }
        case BSON_BOOL: {
            int32_t val = bson_iter_bool(&iter, NULL);
            if (val) {
                binary_set_binary(str, "true", strlen("true"));
            } else {
                binary_set_binary(str, "false", strlen("false"));
            }
            break;
        }
        case BSON_TIMESTAMP: {
            inc = 0;
            ts = bson_iter_timestamp(&iter, &inc, NULL);
            binary_set_va(str, "%u %u", inc, ts);
            break;
        }
        case BSON_DATE: {
            int64_t val = bson_iter_date(&iter, NULL);
            binary_set_va(str, "%"PRId64, val);
            break;
        }
        case BSON_INT64: {
            int64_t val = bson_iter_int64(&iter, NULL);
            binary_set_va(str, "%"PRId64, val);
            break;
        }
        case BSON_NULL:
        case BSON_MINKEY:
        case BSON_MAXKEY:
            break;
        case BSON_REGEX: {
            const char *val = bson_iter_regex(&iter, &options, NULL);
            binary_set_binary(str, val, strlen(val));
            binary_set_fill(str, ' ', 4);
            binary_set_binary(str, options, strlen(options));
            break;
        }
        case BSON_INT32: {
            int32_t val = (int32_t)bson_iter_int32(&iter, NULL);
            binary_set_va(str, "%d", val);
            break;
        }
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
    binary_init(&str, NULL, 0, 0);
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
