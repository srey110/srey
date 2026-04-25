#ifndef REDIS_H_
#define REDIS_H_

#include "utils/buffer.h"

// RESP2/RESP3 类型标识字节定义
#define RESP_STRING  '+' // 简单字符串   RESP2  +OK\r\n
#define RESP_ERROR   '-' // 简单错误     RESP2  -Error message\r\n
#define RESP_INTEGER ':' // 整数         RESP2  :[<+|->]<value>\r\n
#define RESP_NIL     '_' // 空值         RESP3  _\r\n
#define RESP_BOOL    '#' // 布尔值       RESP3  #<t|f>\r\n
#define RESP_DOUBLE  ',' // 浮点数       RESP3  ,[<+|->]<integral>[.<fractional>][<E|e>[sign]<exponent>]\r\n
#define RESP_BIGNUM  '(' // 大整数       RESP3  ([+|-]<number>\r\n

#define RESP_BSTRING '$' // 批量字符串   RESP2  $<length>\r\n<data>\r\n
#define RESP_BERROR  '!' // 批量错误     RESP3  !<length>\r\n<error>\r\n
#define RESP_VERB    '=' // 带编码字符串 RESP3  =<length>\r\n<encoding>:<data>\r\n

#define RESP_ARRAY   '*' // 数组         RESP2  *<number-of-elements>\r\n<element-1>...<element-n>
#define RESP_SET     '~' // 集合         RESP3  ~<number-of-elements>\r\n<element-1>...<element-n>
#define RESP_PUSHE   '>' // 推送         RESP3  ><number-of-elements>\r\n<element-1>...<element-n>
#define RESP_MAP     '%' // 映射         RESP3  %<number-of-entries>\r\n<key-1><value-1>...<key-n><value-n>
#define RESP_ATTR    '|' // 属性类型     RESP3  格式同 Map，但首字节为 |，客户端不应将其视为响应的一部分，
                         //              而是作为用于增强响应的辅助数据
// Redis RESP 协议数据包上下文，链表节点（复合类型如数组由 next 串联）
typedef struct redis_pack_ctx {
    int32_t prot;               // RESP 类型标识字节（见 RESP_* 宏）
    char venc[4];               // 仅 RESP_VERB 使用：3 字节编码名称（如 "txt"），末位为 '\0'
    int64_t nelem;              // 聚合类型（RESP_ARRAY/SET/PUSHE/MAP/ATTR）的元素数量
    int64_t len;                // data 字段的数据长度（字节数），-1 表示 Null bulk
    int64_t ival;               // 整型值（RESP_INTEGER/RESP_BOOL/RESP_BIGNUM）
    double dval;                // 浮点值（RESP_DOUBLE）
    struct redis_pack_ctx *next; // 指向下一个节点（聚合类型链式存储）
    char data[0];               // 字符串数据（RESP_STRING/RESP_ERROR/RESP_BSTRING/RESP_BERROR/RESP_VERB）
}redis_pack_ctx;

// 释放 redis_pack_ctx 链表（含所有 next 节点）
void _redis_pkfree(redis_pack_ctx *pack);
// 释放与 ud_cxt 关联的 Redis 解包上下文
void _redis_udfree(ud_cxt *ud);
/// <summary>
/// 构造 Redis RESP 请求包
/// </summary>
/// <param name="size">输出：请求包长度</param>
/// <param name="fmt">格式字符串，参数以空格分隔；%b 表示二进制（需跟 size_t 长度参数），%% 表示 %，其余同 C printf</param>
/// <param name="...">可变参数</param>
/// <returns>请求包数据（调用方负责释放）</returns>
char *redis_pack(size_t *size, const char *fmt, ...);
/// <summary>
/// Redis RESP 解包：从缓冲区解析一条完整响应（含嵌套聚合类型）
/// </summary>
/// <param name="buf">接收缓冲区</param>
/// <param name="ud">ud_cxt 指针，内部维护解包中间状态</param>
/// <param name="status">输出：解包状态标志，见 prot_status</param>
/// <returns>解析完成的 redis_pack_ctx 链表头，数据不足或出错返回 NULL</returns>
redis_pack_ctx *redis_unpack(buffer_ctx *buf, ud_cxt *ud, int32_t *status);

#endif//REDIS_H_
