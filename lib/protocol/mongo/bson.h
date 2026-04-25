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

/// <summary>
/// 全局初始化：生成 OID 头部信息（主机名哈希+PID）和空 BSON 文档
/// </summary>
void bson_globle_init(void);
/// <summary>
/// 生成一个新的 ObjectId
/// </summary>
/// <param name="oid">输出缓冲区，长度为 BSON_OID_LENS(12)</param>
void bson_oid(char oid[BSON_OID_LENS]);
/// <summary>
/// 返回一个空 BSON 文档（只含长度头和结束符）
/// </summary>
/// <param name="lens">输出文档长度</param>
/// <returns>空 BSON 文档数据指针（静态存储，勿释放）</returns>
const char *bson_empty(size_t *lens);
/// <summary>
/// 初始化 bson_ctx。data 为 NULL 时自动分配内存并开始写入根文档；
/// data 非 NULL 时以只读/追加模式附加到已有数据
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="data">已有 BSON 数据缓冲区，NULL 表示新建</param>
/// <param name="lens">data 的长度</param>
void bson_init(bson_ctx *bson, char *data, size_t lens);
/// <summary>
/// 检查 BSON 是否已完整写入（depth 为 0 且有内容）
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <returns>非零表示已完整写入</returns>
int32_t bson_complete(bson_ctx *bson);
/// <summary>
/// 将 BSON 类型枚举转换为可读字符串
/// </summary>
/// <param name="type">bson_type</param>
/// <returns>类型名称字符串</returns>
const char *bson_type_tostring(bson_type type);
/// <summary>
/// 将 BSON 二进制子类型枚举转换为可读字符串
/// </summary>
/// <param name="type">bson_subtype</param>
/// <returns>子类型名称字符串</returns>
const char *bson_subtype_tostring(bson_subtype type);
/// <summary>
/// 将 bson_ctx 中的文档转换为可读字符串（需调用者 FREE）
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <returns>格式化后的字符串，需 FREE</returns>
char *bson_tostring(bson_ctx *bson);
/// <summary>
/// 将原始 BSON 数据转换为可读字符串（需调用者 FREE）
/// </summary>
/// <param name="data">BSON 原始数据</param>
/// <param name="lens">数据长度</param>
/// <returns>格式化后的字符串，需 FREE</returns>
char *bson_tostring2(char *data, size_t lens);
/// <summary>
/// 将另一个已完成的 BSON 文档的内容（不含外层包装）拼接到当前文档（bson 未写完前使用）
/// </summary>
/// <param name="bson">目标 bson_ctx</param>
/// <param name="doc">源 BSON 文档数据</param>
void bson_cat(bson_ctx *bson, char *doc);
/// <summary>
/// 开始写入一个嵌套文档字段，须配对调用 bson_append_end 结束
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="key">字段名</param>
void bson_append_document_begain(bson_ctx *bson, const char *key);
/// <summary>
/// 开始写入一个数组字段，须配对调用 bson_append_end 结束
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="key">字段名</param>
void bson_append_array_begain(bson_ctx *bson, const char *key);
/// <summary>
/// 结束写入当前层级的文档或数组（写入 EOD 并回填长度）
/// </summary>
/// <param name="bson">bson_ctx</param>
void bson_append_end(bson_ctx *bson);
/// <summary>
/// 追加 double 类型字段
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="key">字段名</param>
/// <param name="val">double 值</param>
void bson_append_double(bson_ctx *bson, const char *key, double val);
/// <summary>
/// 追加 UTF-8 字符串类型字段
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="key">字段名</param>
/// <param name="val">字符串值</param>
void bson_append_utf8(bson_ctx *bson, const char *key, const char *val);
/// <summary>
/// 追加已序列化的 BSON 文档类型字段
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="key">字段名</param>
/// <param name="doc">BSON 文档数据</param>
/// <param name="lens">文档数据长度</param>
void bson_append_document(bson_ctx *bson, const char *key, char *doc, size_t lens);
/// <summary>
/// 追加已序列化的 BSON 数组类型字段
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="key">字段名</param>
/// <param name="doc">BSON 数组数据</param>
/// <param name="lens">数据长度</param>
void bson_append_array(bson_ctx *bson, const char *key, char *doc, size_t lens);
/// <summary>
/// 追加二进制数据类型字段
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="key">字段名</param>
/// <param name="type">bson_subtype 子类型</param>
/// <param name="val">二进制数据</param>
/// <param name="lens">数据长度</param>
void bson_append_binary(bson_ctx *bson, const char *key, bson_subtype type, char *val, size_t lens);
/// <summary>
/// 追加 ObjectId 类型字段
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="key">字段名</param>
/// <param name="oid">12 字节 ObjectId 数据</param>
void bson_append_oid(bson_ctx *bson, const char *key, char oid[BSON_OID_LENS]);
/// <summary>
/// 追加布尔类型字段
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="key">字段名</param>
/// <param name="b">0 为 false，非零为 true</param>
void bson_append_bool(bson_ctx *bson, const char *key, int8_t b);
/// <summary>
/// 追加 UTC 时间戳类型字段（int64，毫秒）
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="key">字段名</param>
/// <param name="date">UTC 毫秒时间戳</param>
void bson_append_date(bson_ctx *bson, const char *key, int64_t date);
/// <summary>
/// 追加 null 类型字段
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="key">字段名</param>
void bson_append_null(bson_ctx *bson, const char *key);
/// <summary>
/// 追加正则表达式类型字段
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="key">字段名</param>
/// <param name="pattern">正则模式字符串</param>
/// <param name="options">正则选项字符串</param>
void bson_append_regex(bson_ctx *bson, const char *key, const char *pattern, const char *options);
/// <summary>
/// 追加 JavaScript 代码类型字段
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="key">字段名</param>
/// <param name="jscode">JavaScript 代码字符串</param>
void bson_append_jscode(bson_ctx *bson, const char *key, const char *jscode);
/// <summary>
/// 追加 int32 类型字段
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="key">字段名</param>
/// <param name="val">int32 值</param>
void bson_append_int32(bson_ctx *bson, const char *key, int32_t val);
/// <summary>
/// 追加时间戳类型字段（低4字节为自增量，高4字节为秒级时间戳）
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="key">字段名</param>
/// <param name="ts">秒级时间戳</param>
/// <param name="inc">自增量</param>
void bson_append_timestamp(bson_ctx *bson, const char *key, uint32_t ts, uint32_t inc);
/// <summary>
/// 追加 int64 类型字段
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="key">字段名</param>
/// <param name="val">int64 值</param>
void bson_append_int64(bson_ctx *bson, const char *key, int64_t val);
/// <summary>
/// 追加 MinKey 类型字段（BSON 中最小值标记）
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="key">字段名</param>
void bson_append_minkey(bson_ctx *bson, const char *key);
/// <summary>
/// 追加 MaxKey 类型字段（BSON 中最大值标记）
/// </summary>
/// <param name="bson">bson_ctx</param>
/// <param name="key">字段名</param>
void bson_append_maxkey(bson_ctx *bson, const char *key);
/// <summary>
/// 初始化迭代器，绑定到指定 bson_ctx
/// </summary>
/// <param name="iter">bson_iter</param>
/// <param name="bson">bson_ctx</param>
void bson_iter_init(bson_iter *iter, bson_ctx *bson);
/// <summary>
/// 将迭代器重置到文档起始位置
/// </summary>
/// <param name="iter">bson_iter</param>
void bson_iter_reset(bson_iter *iter);
/// <summary>
/// 迭代到下一个字段，返回非零表示有值，0 表示遍历结束
/// </summary>
/// <param name="iter">bson_iter</param>
/// <returns>非零表示有值</returns>
int32_t bson_iter_next(bson_iter *iter);
/// <summary>
/// 从当前迭代器位置查找指定键，支持点分多级路径（如 "a.b.c"）
/// </summary>
/// <param name="iter">起始迭代器</param>
/// <param name="keys">点分键路径，如 "cursor.id"</param>
/// <param name="result">找到时输出结果迭代器</param>
/// <returns>ERR_OK 找到，ERR_FAILED 未找到</returns>
int32_t bson_iter_find(bson_iter *iter, const char *keys, bson_iter *result);
/// <summary>
/// 从迭代器当前字段读取 double 值
/// </summary>
/// <param name="iter">bson_iter（类型须为 BSON_DOUBLE）</param>
/// <param name="err">输出错误码，NULL 则忽略</param>
/// <returns>double 值</returns>
double bson_iter_double(bson_iter *iter, int32_t *err);
/// <summary>
/// 从迭代器当前字段读取 UTF-8 字符串
/// </summary>
/// <param name="iter">bson_iter（类型须为 BSON_UTF8）</param>
/// <param name="err">输出错误码，NULL 则忽略</param>
/// <returns>字符串指针（指向内部缓冲，无需释放）</returns>
const char *bson_iter_utf8(bson_iter *iter, int32_t *err);
/// <summary>
/// 从迭代器当前字段读取嵌套文档数据
/// </summary>
/// <param name="iter">bson_iter（类型须为 BSON_DOCUMENT）</param>
/// <param name="lens">输出文档数据长度</param>
/// <param name="err">输出错误码，NULL 则忽略</param>
/// <returns>BSON 文档数据指针</returns>
char *bson_iter_document(bson_iter *iter, size_t *lens, int32_t *err);
/// <summary>
/// 从迭代器当前字段读取数组数据
/// </summary>
/// <param name="iter">bson_iter（类型须为 BSON_ARRAY）</param>
/// <param name="lens">输出数组数据长度</param>
/// <param name="err">输出错误码，NULL 则忽略</param>
/// <returns>BSON 数组数据指针</returns>
char *bson_iter_array(bson_iter *iter, size_t *lens, int32_t *err);
/// <summary>
/// 从迭代器当前字段读取二进制数据
/// </summary>
/// <param name="iter">bson_iter（类型须为 BSON_BINARY）</param>
/// <param name="subtype">输出子类型，NULL 则忽略</param>
/// <param name="lens">输出数据长度</param>
/// <param name="err">输出错误码，NULL 则忽略</param>
/// <returns>二进制数据指针</returns>
char *bson_iter_binary(bson_iter *iter, bson_subtype *subtype, size_t *lens, int32_t *err);
/// <summary>
/// 从迭代器当前字段读取 ObjectId
/// </summary>
/// <param name="iter">bson_iter（类型须为 BSON_OID）</param>
/// <param name="err">输出错误码，NULL 则忽略</param>
/// <returns>12 字节 OID 数据指针</returns>
char *bson_iter_oid(bson_iter *iter, int32_t *err);
/// <summary>
/// 从迭代器当前字段读取布尔值
/// </summary>
/// <param name="iter">bson_iter（类型须为 BSON_BOOL）</param>
/// <param name="err">输出错误码，NULL 则忽略</param>
/// <returns>0 为 false，非零为 true</returns>
int32_t bson_iter_bool(bson_iter *iter, int32_t *err);
/// <summary>
/// 从迭代器当前字段读取 UTC 毫秒时间戳
/// </summary>
/// <param name="iter">bson_iter（类型须为 BSON_DATE）</param>
/// <param name="err">输出错误码，NULL 则忽略</param>
/// <returns>UTC 毫秒时间戳</returns>
int64_t bson_iter_date(bson_iter *iter, int32_t *err);
/// <summary>
/// 从迭代器当前字段读取正则表达式
/// </summary>
/// <param name="iter">bson_iter（类型须为 BSON_REGEX）</param>
/// <param name="options">输出正则选项字符串指针，NULL 则忽略</param>
/// <param name="err">输出错误码，NULL 则忽略</param>
/// <returns>正则模式字符串指针</returns>
const char *bson_iter_regex(bson_iter *iter, char **options, int32_t *err);
/// <summary>
/// 从迭代器当前字段读取 JavaScript 代码字符串
/// </summary>
/// <param name="iter">bson_iter（类型须为 BSON_JSCODE）</param>
/// <param name="err">输出错误码，NULL 则忽略</param>
/// <returns>JavaScript 代码字符串指针</returns>
const char *bson_iter_jscode(bson_iter *iter, int32_t *err);
/// <summary>
/// 从迭代器当前字段读取 int32 值
/// </summary>
/// <param name="iter">bson_iter（类型须为 BSON_INT32）</param>
/// <param name="err">输出错误码，NULL 则忽略</param>
/// <returns>int32 值</returns>
int32_t bson_iter_int32(bson_iter *iter, int32_t *err);
/// <summary>
/// 从迭代器当前字段读取时间戳（低4字节为自增量，高4字节为秒级时间戳）
/// </summary>
/// <param name="iter">bson_iter（类型须为 BSON_TIMESTAMP）</param>
/// <param name="inc">输出自增量</param>
/// <param name="err">输出错误码，NULL 则忽略</param>
/// <returns>秒级时间戳</returns>
uint32_t bson_iter_timestamp(bson_iter *iter, uint32_t *inc, int32_t *err);
/// <summary>
/// 从迭代器当前字段读取 int64 值
/// </summary>
/// <param name="iter">bson_iter（类型须为 BSON_INT64）</param>
/// <param name="err">输出错误码，NULL 则忽略</param>
/// <returns>int64 值</returns>
int64_t bson_iter_int64(bson_iter *iter, int32_t *err);

#endif//BSON_H_
