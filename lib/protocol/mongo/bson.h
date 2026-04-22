#ifndef BSON_H_
#define BSON_H_

#include "utils/binary.h"

#define BSON_OID_LENS  12//ObjectId长度
#define BSON_MAX_DEPTH 18//最大嵌套层数

#define BSON_DOC(bson)  (bson)->doc.data
#define BSON_DOC_LENS(bson)  (bson)->doc.offset
#define BSON_FREE(bson)  FREE((bson)->doc.data)

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

typedef struct bson_ctx {
    int32_t depth;//当前层级
    binary_ctx doc;
    size_t offsets[BSON_MAX_DEPTH];//层级开始位置
}bson_ctx;
typedef struct bson_iter {
    bson_type type;
    bson_subtype subtype;
    size_t doclens;//文档长度
    size_t lens;//val长度
    const char *key;//key
    char *val;//值1
    char *val2;//值2
    binary_ctx *doc;//bson_ctx中的doc
} bson_iter;

//初始化oid empty bson
void bson_globle_init(void);
//获取oid
void bson_oid(char iod[BSON_OID_LENS]);
//返回空bson
const char *bson_empty(size_t *lens);
//初始化
void bson_init(bson_ctx *bson, char *data, size_t lens);
//检查bson是否写完
int32_t bson_complete(bson_ctx *bson);
//类型转字符串
const char *bson_type_tostring(bson_type type);
const char *bson_subtype_tostring(bson_subtype type);
//bson转换成字符串 需要FREE
char *bson_tostring(bson_ctx *bson);
char *bson_tostring2(char *data, size_t lens);
//拼接bson(bson未写完前)
void bson_cat(bson_ctx *bson, char *doc);
//开始写入对象 bson_append_end 结束写入
void bson_append_document_begain(bson_ctx *bson, const char *key);
//开始写入数组 bson_append_end 结束写入
void bson_append_array_begain(bson_ctx *bson, const char *key);
//结束写入
void bson_append_end(bson_ctx *bson);
void bson_append_double(bson_ctx *bson, const char *key, double val);
void bson_append_utf8(bson_ctx *bson, const char *key, const char *val);
void bson_append_document(bson_ctx *bson, const char *key, char *doc, size_t lens);
void bson_append_array(bson_ctx *bson, const char *key, char *doc, size_t lens);
void bson_append_binary(bson_ctx *bson, const char *key, bson_subtype type, char *val, size_t lens);
void bson_append_oid(bson_ctx *bson, const char *key, char oid[BSON_OID_LENS]);
void bson_append_bool(bson_ctx *bson, const char *key, int8_t b);
void bson_append_date(bson_ctx *bson, const char *key, int64_t date);
void bson_append_null(bson_ctx *bson, const char *key);
void bson_append_regex(bson_ctx *bson, const char *key, const char *pattern, const char *options);
void bson_append_jscode(bson_ctx *bson, const char *key, const char *jscode);
void bson_append_int32(bson_ctx *bson, const char *key, int32_t val);
void bson_append_timestamp(bson_ctx *bson, const char *key, uint32_t ts, uint32_t inc);
void bson_append_int64(bson_ctx *bson, const char *key, int64_t val);
void bson_append_minkey(bson_ctx *bson, const char *key);
void bson_append_maxkey(bson_ctx *bson, const char *key);
//iterator初始化
void bson_iter_init(bson_iter *iter, bson_ctx *bson);
//重置到开始位置
void bson_iter_reset(bson_iter *iter);
//遍历 true有值
int32_t bson_iter_next(bson_iter *iter);
//从iter处开始查找 keys: key1.key2...   ERR_OK 找到
int32_t bson_iter_find(bson_iter *iter, const char *keys, bson_iter *result);
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
