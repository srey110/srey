#ifndef BSON_H_
#define BSON_H_

#include "utils/binary.h"

#define BSON_OID_LENS 12
//e_name	::=	cstring
//string	::=	int32 (byte*) unsigned_byte(0) 
//cstring	:: = (byte*) unsigned_byte(0)
//binary	::=	int32 subtype (byte*)
//array ['red', 'blue'] encodes as the document {'0': 'red', '1': 'blue'}.
typedef enum bson_type {
    BSON_EOD = 0x00,//结尾
    BSON_DOUBLE = 0x01,//double              signed_byte(1) e_name double
    BSON_UTF8 = 0x02,//字符串                signed_byte(2) e_name string 
    BSON_DOCUMENT = 0x03,//对象              signed_byte(3) e_name document
    BSON_ARRAY = 0x04,//阵列                 signed_byte(4) e_name document
    BSON_BINARY = 0x05,//二进制数据          signed_byte(5) e_name binary
    BSON_OID = 0x07,//ObjectId               signed_byte(7) e_name (byte*12)
    BSON_BOOL = 0x08,//布尔                  signed_byte(8) e_name unsigned_byte(0/1)
    BSON_DATE = 0x09,//Date                  signed_byte(9) e_name int64  UTC datetime. int64 is UTC milliseconds since the Unix epoch
    BSON_NULL = 0x0A,//null                  signed_byte(10) e_name
    BSON_REGEX = 0x0B,//正则表达式           signed_byte(11) e_name cstring(regex pattern) cstring(regex options)
    BSON_JSCODE = 0x0D,//JavaScript          signed_byte(13) e_name string
    BSON_INT32 = 0x10,//32 位整数            signed_byte(16) e_name int32
    BSON_TIMESTAMP = 0x11,//时间戳           signed_byte(17) e_name uint64 The first 4 bytes are an increment and the second 4 bytes are a timestamp.
    BSON_INT64 = 0x12,//64 位整型            signed_byte(18) e_name int64
    BSON_DECIMAL128 = 0x13,//Decimal128
    BSON_MAXKEY = 0x7F,//Max key             signed_byte(-1) e_name
    BSON_MINKEY = 0xFF,//Min key             signed_byte(127) e_name
} bson_type;
typedef enum bson_subtype {
    BSON_SUBTYPE_BINARY = 0x00,//通用二进制子类型
    BSON_SUBTYPE_FUNCTION = 0x01,//函数数据
    BSON_SUBTYPE_UUID = 0x04,//UUID
    BSON_SUBTYPE_MD5 = 0x05,//MD5
    BSON_SUBTYPE_ENCRYPTED = 0x06,//加密的 BSON 值
    BSON_SUBTYPE_COMPRESSED = 0x07,//压缩
    BSON_SUBTYPE_SENSITIVE = 0x08,//敏感数据
    BSON_SUBTYPE_VECTOR = 0x09,//向量数据是由相同类型的数字组成的密集数组
    BSON_SUBTYPE_USER = 0x80,//自定义数据
} bson_subtype;
typedef struct bson_iter {
    bson_type type;
    bson_subtype subtype;
    size_t doclens;
    size_t lens;
    const char *key;
    char *val;
    char *val2;
    binary_ctx *bson;
} bson_iter;

void bson_init(binary_ctx *bson, char *data, size_t lens);
const char *bson_type_tostring(bson_type type);
const char *bson_subtype_tostring(bson_subtype type);
char *bson_tostring(binary_ctx *bson);
void bson_append_start(binary_ctx *bson);
void bson_append_end(binary_ctx *bson);
void bson_append_double(binary_ctx *bson, const char *key, double val);
void bson_append_utf8(binary_ctx *bson, const char *key, const char *val);
void bson_append_document(binary_ctx *bson, const char *key, char *doc, size_t lens);
void bson_append_array(binary_ctx *bson, const char *key, char *doc, size_t lens);
void bson_append_binary(binary_ctx *bson, const char *key, bson_subtype type, char *val, size_t lens);
void bson_append_oid(binary_ctx *bson, const char *key, char oid[BSON_OID_LENS]);
void bson_append_bool(binary_ctx *bson, const char *key, int8_t b);
void bson_append_date(binary_ctx *bson, const char *key, int64_t date);
void bson_append_null(binary_ctx *bson, const char *key);
void bson_append_regex(binary_ctx *bson, const char *key, const char *pattern, const char *options);
void bson_append_jscode(binary_ctx *bson, const char *key, const char *jscode);
void bson_append_int32(binary_ctx *bson, const char *key, int32_t val);
void bson_append_timestamp(binary_ctx *bson, const char *key, uint32_t ts, uint32_t inc);
void bson_append_int64(binary_ctx *bson, const char *key, int64_t val);
void bson_append_minkey(binary_ctx *bson, const char *key);
void bson_append_maxkey(binary_ctx *bson, const char *key);

void bson_iter_init(bson_iter *iter, binary_ctx *bson);
int32_t bson_iter_next(bson_iter *iter);
double bson_iter_double(bson_iter *iter, int32_t *err);
const char *bson_iter_utf8(bson_iter *iter, int32_t *err);
char *bson_iter_document(bson_iter *iter, size_t *lens, int32_t *err);
char *bson_iter_array(bson_iter *iter, size_t *lens, int32_t *err);
char *bson_iter_binary(bson_iter *iter, bson_subtype *subtype, size_t *lens, int32_t *err);
char *bson_iter_oid(bson_iter *iter, int32_t *err);
int32_t bson_iter_bool(bson_iter *iter, int32_t *err);
int64_t bson_iter_date(bson_iter *iter, int32_t *err);
const char *bson_iter_regex(bson_iter *iter, char **options, int32_t *err);
const char *bson_iter_jscode(bson_iter *iter, int32_t *err);
int32_t bson_iter_int32(bson_iter *iter, int32_t *err);
uint32_t bson_iter_timestamp(bson_iter *iter, uint32_t *inc, int32_t *err);
int64_t bson_iter_int64(bson_iter *iter, int32_t *err);

#endif//BSON_H_
