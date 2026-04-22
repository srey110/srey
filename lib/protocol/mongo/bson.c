#include "protocol/mongo/bson.h"

#define BSON_APPEND_CSTRING(str) binary_set_string(&bson->doc, str, 0)
#define BSON_APPEND_KEY(type) \
    binary_set_int8(&bson->doc, type);\
    BSON_APPEND_CSTRING(key)
#define BSON_APPEND_STRING(str)\
    binary_set_integer(&bson->doc, strlen(str) + 1, 4, 1);\
    binary_set_string(&bson->doc, str, 0)

static char _bson_empty[5] = { 0 };
static uint8_t _oid_header[5];
static atomic_t _oid_counter = 0;

static void _bson_oid_init(void) {
    int32_t pid = GETPID();
    uint32_t h = 0;
    char hostname[HOST_LENS];
    if (0 == gethostname(hostname, sizeof(hostname))) {
        int32_t i;
        for (i = 0; i < sizeof(hostname) && hostname[i]; i++) {
            h = h ^ ((h << 5) + (h >> 2) + hostname[i]);
        }
        h ^= i;
    }
    _oid_header[0] = h & 0xff;
    _oid_header[1] = (h >> 8) & 0xff;
    _oid_header[2] = (h >> 16) & 0xff;
    _oid_header[3] = pid & 0xff;
    _oid_header[4] = (pid >> 8) & 0xff;
    _oid_counter = randrange(10000, 20000);
}
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
    oid[0] = (ti >> 24) & 0xff;
    oid[1] = (ti >> 16) & 0xff;
    oid[2] = (ti >> 8) & 0xff;
    oid[3] = ti & 0xff;
    memcpy(oid + 4, _oid_header, 5);
    oid[9] = (id >> 16) & 0xff;
    oid[10] = (id >> 8) & 0xff;
    oid[11] = id & 0xff;
}
const char *bson_empty(size_t *lens) {
    SET_PTR(lens, 5);
    return _bson_empty;
}
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
        ZERO(bson->offsets, sizeof(bson->offsets));
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
void bson_cat(bson_ctx *bson, char *doc) {
    if (NULL == doc) {
        return;
    }
    uint32_t lens = (uint32_t)unpack_integer(doc, 4, 1, 0);
    if (lens <= 5) {
        return;
    }
    binary_set_string(&bson->doc, doc + 4, lens - 5);//4 + 1(eod)
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
void bson_append_utf8(bson_ctx *bson, const char *key, const char *val) {
    BSON_APPEND_KEY(BSON_UTF8);
    BSON_APPEND_STRING(val);
}
//signed_byte(3) e_name document
void bson_append_document(bson_ctx *bson, const char *key, char *doc, size_t lens) {
    BSON_APPEND_KEY(BSON_DOCUMENT);
    binary_set_string(&bson->doc, doc, lens);
}
//signed_byte(4) e_name document
void bson_append_array(bson_ctx *bson, const char *key, char *doc, size_t lens) {
    BSON_APPEND_KEY(BSON_ARRAY);
    binary_set_string(&bson->doc, doc, lens);
}
//signed_byte(5) e_name binary
void bson_append_binary(bson_ctx *bson, const char *key, bson_subtype type, char *val, size_t lens) {
    BSON_APPEND_KEY(BSON_BINARY);
    binary_set_integer(&bson->doc, lens, 4, 1);
    binary_set_int8(&bson->doc, type);
    binary_set_string(&bson->doc, val, lens);
}
//signed_byte(7) e_name (byte*12)
void bson_append_oid(bson_ctx *bson, const char *key, char oid[BSON_OID_LENS]) {
    BSON_APPEND_KEY(BSON_OID);
    binary_set_string(&bson->doc, oid, BSON_OID_LENS);
}
//signed_byte(8) e_name unsigned_byte(0/1)
void bson_append_bool(bson_ctx *bson, const char *key, int8_t b) {
    BSON_APPEND_KEY(BSON_BOOL);
    if (b) {
        binary_set_int8(&bson->doc, 1);
    }
    else {
        binary_set_int8(&bson->doc, 0);
    }
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
//signed_byte(13) e_name string
void bson_append_jscode(bson_ctx *bson, const char *key, const char *jscode) {
    BSON_APPEND_KEY(BSON_JSCODE);
    BSON_APPEND_STRING(jscode);
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
void bson_iter_init(bson_iter *iter, bson_ctx *bson) {
    iter->doc = &bson->doc;
    iter->doclens = binary_get_integer(iter->doc, 4, 1);
}
static void _bson_iter_clear(bson_iter *iter) {
    iter->subtype = 0;
    iter->lens = 0;
    iter->key = NULL;
    iter->val = NULL;
    iter->val2 = NULL;
}
void bson_iter_reset(bson_iter *iter) {
    binary_offset(iter->doc, 4);
}
int32_t bson_iter_next(bson_iter *iter) {
    size_t off;
    int32_t more = 1;
    _bson_iter_clear(iter);
    iter->type = (uint8_t)binary_get_int8(iter->doc);//signed_byte(type)
    switch (iter->type) {
    case BSON_EOD:
        more = 0;
        break;
    case BSON_DOUBLE://e_name double
        iter->key = binary_get_string(iter->doc, 0);
        iter->lens = sizeof(double);
        iter->val = binary_get_string(iter->doc, iter->lens);
        break;
    case BSON_UTF8://e_name string
    case BSON_JSCODE://e_name string
        iter->key = binary_get_string(iter->doc, 0);
        iter->lens = binary_get_integer(iter->doc, 4, 1) - 1;
        iter->val = binary_get_string(iter->doc, iter->lens + 1);
        break;
    case BSON_DOCUMENT://e_name document
    case BSON_ARRAY://e_name document
        iter->key = binary_get_string(iter->doc, 0);
        off = iter->doc->offset;
        iter->lens = binary_get_integer(iter->doc, 4, 1);
        binary_offset(iter->doc, off);
        iter->val = binary_get_string(iter->doc, iter->lens);
        break;
    case BSON_BINARY://e_name binary
        iter->key = binary_get_string(iter->doc, 0);
        iter->lens = binary_get_integer(iter->doc, 4, 1);
        iter->subtype = binary_get_int8(iter->doc);
        iter->val = binary_get_string(iter->doc, iter->lens);
        break;
    case BSON_OID://e_name (byte*12)
        iter->key = binary_get_string(iter->doc, 0);
        iter->lens = BSON_OID_LENS;
        iter->val = binary_get_string(iter->doc, iter->lens);
        break;
    case BSON_BOOL://e_name unsigned_byte(0/1)
        iter->key = binary_get_string(iter->doc, 0);
        iter->lens = 1;
        iter->val = binary_get_string(iter->doc, iter->lens);
        break;
    case BSON_DATE://e_name int64
    case BSON_TIMESTAMP://e_name uint64
    case BSON_INT64://e_name int64
        iter->key = binary_get_string(iter->doc, 0);
        iter->lens = 8;
        iter->val = binary_get_string(iter->doc, iter->lens);
        break;
    case BSON_NULL://e_name
    case BSON_MINKEY://e_name
    case BSON_MAXKEY://e_name
        iter->key = binary_get_string(iter->doc, 0);
        break;
    case BSON_REGEX://e_name cstring(regex pattern) cstring(regex options)
        iter->key = binary_get_string(iter->doc, 0);
        iter->val = binary_get_string(iter->doc, 0);
        iter->val2 = binary_get_string(iter->doc, 0);
        break;
    case BSON_INT32://e_name int32
        iter->key = binary_get_string(iter->doc, 0);
        iter->lens = 4;
        iter->val = binary_get_string(iter->doc, iter->lens);
        break;
    case BSON_DECIMAL128:
    default:
        more = 0;
        LOG_WARN("unsupported bson type %d.", iter->type);
        break;
    }
    return more;
}
static int32_t _bson_iter_find(bson_iter *iter, const char *key, size_t klens, bson_iter *result) {
    while (bson_iter_next(iter)) {
        if (klens == strlen(iter->key)
            && 0 == memcmp(iter->key, key, klens)) {
            *result = *iter;
            return ERR_OK;
        }
    }
    return ERR_FAILED;
}
int32_t bson_iter_find(bson_iter *iter, const char *keys, bson_iter *result) {
    int32_t rtn;
    size_t klens = strlen(keys);
    size_t offset = iter->doc->offset;
    if (NULL == strstr(keys, ".")) {
        rtn = _bson_iter_find(iter, keys, klens, result);
        binary_offset(iter->doc, offset);
        return rtn;
    }
    size_t n;
    buf_ctx *arr_key = split(keys, klens, ".", 1, &n);
    bson_iter cur_iter = *iter;
    buf_ctx *key;
    bson_ctx bson;
    for (size_t i = 0; i < n; i++) {
        key = &arr_key[i];
        if (NULL == key->data
            || 0 == key->lens) {
            rtn = ERR_FAILED;
            break;
        }
        rtn = _bson_iter_find(&cur_iter, key->data, key->lens, result);
        if (ERR_OK != rtn) {
            break;
        }
        if (i == n - 1) {//最后一层
            break;
        }
        if (BSON_DOCUMENT != result->type
            && BSON_ARRAY != result->type) {
            rtn = ERR_FAILED;
            break;
        }
        bson_init(&bson, result->val, result->lens);
        bson_iter_init(&cur_iter, &bson);
    }
    FREE(arr_key);
    binary_offset(iter->doc, offset);
    return rtn;
}
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
static void _bson_dump(bson_ctx *bson, int32_t index, binary_ctx *str) {
    bson_iter iter;
    bson_iter_init(&iter, bson);
    while (bson_iter_next(&iter)) {
        binary_set_fill(str, ' ', index * 4);
        binary_set_string(str, iter.key, strlen(iter.key));
        binary_set_string(str, "(", 1);
        const char *strtype = bson_type_tostring(iter.type);
        binary_set_string(str, strtype, strlen(strtype));
        binary_set_string(str, ")", 1);
        binary_set_string(str, ": ", 2);
        switch (iter.type) {
        case BSON_DOUBLE: {
            double val = bson_iter_double(&iter, NULL);
            binary_set_va(str, "%lf", val);
            break;
        }
        case BSON_UTF8: {
            const char *val = bson_iter_utf8(&iter, NULL);
            binary_set_string(str, val, strlen(val));
            break;
        }
        case BSON_JSCODE: {
            const char *val = bson_iter_jscode(&iter, NULL);
            binary_set_string(str, val, strlen(val));
            break;
        }
        case BSON_DOCUMENT:
        case BSON_ARRAY: {
            bson_ctx child;
            bson_init(&child, iter.val, iter.lens);
            binary_ctx strchild;
            binary_init(&strchild, NULL, 0, 0);
            if (BSON_DOCUMENT == iter.type) {
                binary_set_string(&strchild, "{\r\n", 3);
            }
            else {
                binary_set_string(&strchild, "[\r\n", 3);
            }
            _bson_dump(&child, index + 1, &strchild);
            binary_set_fill(&strchild, ' ', index * 4);
            if (BSON_DOCUMENT == iter.type) {
                binary_set_string(&strchild, "}", 1);
            }
            else {
                binary_set_string(&strchild, "]", 1);
            }
            binary_set_string(str, strchild.data, strchild.offset);
            FREE(strchild.data);
            break;
        }
        case BSON_BINARY: {
            size_t lens;
            bson_subtype subtype;
            char *val = bson_iter_binary(&iter, &subtype, &lens, NULL);
            const char *subtstr = bson_subtype_tostring(subtype);
            binary_set_string(str, "(", 1);
            binary_set_string(str, subtstr, strlen(subtstr));
            binary_set_string(str, ") ", 2);
            char *hex;
            MALLOC(hex, HEX_ENSIZE(lens));
            tohex(val, lens, hex);
            binary_set_string(str, hex, strlen(hex));
            FREE(hex);
            break;
        }
        case BSON_OID: {
            char *val = bson_iter_oid(&iter, NULL);
            char hex[HEX_ENSIZE(BSON_OID_LENS)];
            tohex(val, BSON_OID_LENS, hex);
            binary_set_string(str, hex, strlen(hex));
            break;
        }
        case BSON_BOOL: {
            int32_t val = bson_iter_bool(&iter, NULL);
            if (val) {
                binary_set_string(str, "true", strlen("true"));
            }
            else {
                binary_set_string(str, "false", strlen("false"));
            }
            break;
        }
        case BSON_TIMESTAMP: {
            uint32_t inc;
            uint32_t ts = bson_iter_timestamp(&iter, &inc, NULL);
            binary_set_va(str, "%d %d", inc, ts);
            break;
        }
        case BSON_DATE: {
            int64_t val = bson_iter_date(&iter, NULL);
            binary_set_va(str, "%"PRIu64, val);
            break;
        }
        case BSON_INT64: {
            int64_t val = bson_iter_int64(&iter, NULL);
            binary_set_va(str, "%"PRIu64, val);
            break;
        }
        case BSON_NULL:
        case BSON_MINKEY:
        case BSON_MAXKEY:
            break;
        case BSON_REGEX: {
            char *options;
            const char *val = bson_iter_regex(&iter, &options, NULL);
            binary_set_string(str, val, strlen(val));
            binary_set_fill(str, ' ', 4);
            binary_set_string(str, options, strlen(options));
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
        binary_set_string(str, FLAG_CRLF, CRLF_SIZE);
    }
}
char *bson_tostring(bson_ctx *bson) {
    size_t offset = bson->doc.offset;
    binary_offset(&bson->doc, 0);
    binary_ctx str;
    binary_init(&str, NULL, 0, 0);
    binary_set_string(&str, "{\r\n", 3);
    _bson_dump(bson, 1, &str);
    binary_set_string(&str, "}", 1);
    binary_set_int8(&str, 0);
    binary_offset(&bson->doc, offset);
    return str.data;
}
char *bson_tostring2(char *data, size_t lens) {
    bson_ctx bson;
    bson_init(&bson, data, lens);
    return bson_tostring(&bson);
}
