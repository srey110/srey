#ifndef REDIS_H_
#define REDIS_H_

#include "utils/buffer.h"
#include "protocol/prots_pub.h"

//最大聚合嵌套层数：解包的帧栈按它定长，嵌套超限即 PROT_ERROR，保的是帧栈不溢出(内存兜底见 REDIS_MAX_NODES)。
//含 stack[0] 顶层虚拟帧，故实际可嵌套 REDIS_MAX_DEPTH-1 层；上层沿回复链表建嵌套结构时，层栈按它定长不会溢出
#define REDIS_MAX_DEPTH     18
#define REDIS_NUM_CAP       64 // 长度 token 的容量(不含)：最多 63 位
#define REDIS_HDR_PEEK      (REDIS_NUM_CAP + 2) // 元素头本地副本：类型 1 + 长度最多 63 位 + CRLF 2
//单次 RESP 解包最大节点数，纯内存兜底(嵌套深度另由 REDIS_MAX_DEPTH 卡，那道保的是帧栈不溢出)，
//此值只决定为一个未完成回复最多垫多少内存，不应低到误伤合法的扁平大结果集。
//1<<18 时最坏约 17MB（节点约 64B/个），仍是现有 7 万元素用例的 3.7 倍余量；
//再往上抬只是让恶意 server 能用约 4MB 流量顶出几十 MB，业务侧的大结果集应分页取
#define REDIS_MAX_NODES     (1 << 18)

// RESP2/RESP3 类型标识字节定义
#define RESP_STRING  '+' // 简单字符串   RESP2  +OK\r\n
#define RESP_ERROR   '-' // 简单错误     RESP2  -Error message\r\n
#define RESP_INTEGER ':' // 整数         RESP2  :[<+|->]<value>\r\n
#define RESP_NIL     '_' // 空值         RESP3  _\r\n
#define RESP_BOOL    '#' // 布尔值       RESP3  #<t|f>\r\n
#define RESP_DOUBLE  ',' // 浮点数       RESP3  ,[<+|->]<integral>[.<fractional>][<E|e>[sign]<exponent>]\r\n
#define RESP_BIGNUM  '(' // 大整数 按字符串处理       RESP3  ([+|-]<number>\r\n

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
    int64_t ival;               // 整型值（RESP_INTEGER/RESP_BOOL）
    union {
        double dval;            // 浮点值（RESP_DOUBLE）
        struct mem_arena_blk *blocks; // 仅聚合回复的首节点：前几个之后的节点所在的块链(见 redis.c 的 REDIS_SOLO_NODES)，随首节点释放
    };                          // 共用一格是为了结构体头保持 48 字节：data 与单独分配时同样 16 字节对齐
    struct redis_pack_ctx *next; // 指向下一个节点（聚合类型链式存储）
    char data[];               // 字符串数据（RESP_STRING/RESP_ERROR/RESP_BSTRING/RESP_BERROR/RESP_VERB/RESP_BIGNUM）
}redis_pack_ctx;

// 释放 redis_pack_ctx 链表（含所有 next 节点）。只能传一条回复的首节点：中间节点可能在首节点的块链里，不能单独释放
void _redis_pkfree(void *data);
// 释放与 ud_cxt 关联的 Redis 解包上下文
void _redis_udfree(ud_cxt *ud);
// 判断当前回复能否唤醒等待者：RESP3 推送除订阅类确认外都不能(名单见实现)
int32_t _redis_may_resume(void *data);
/// <summary>
/// 构造 Redis RESP 请求包
/// </summary>
/// <param name="size">输出：请求包长度</param>
/// <param name="fmt">格式字符串，参数以空格分隔；%b 表示二进制（需跟 size_t 长度参数），%% 表示 %。
/// 其余支持 %s、%c、%p、diouxX（含 hh/h/l/ll/z 长度修饰）、eEfFgGaA，语义同 C printf。
/// 不在此列的转换（如 %j、%t、%L，以及只有标志没有转换符的末尾 %）不支持，单个转换说明超过 63 字节也不支持</param>
/// <param name="...">可变参数</param>
/// <returns>请求包数据（调用方负责释放）；格式串含不支持的转换时整条拒绝，返回 NULL 且 *size 置 0</returns>
char *redis_pack(size_t *size, const char *fmt, ...);
/// <summary>
/// Redis RESP 解包：从缓冲区解析一条完整响应（含嵌套聚合类型）。只解 RESP 回复，PACK_REDIS 只能用在客户端连接上：
/// 监听侧收到的内联命令(如纯文本 "PING\r\n")首字节不是类型字节，直接判协议错误
/// </summary>
/// <param name="buf">接收缓冲区</param>
/// <param name="ud">ud_cxt 指针，内部维护解包中间状态</param>
/// <param name="size">输出：返回包时写入这条回复所有节点的分配长度之和（含节点头），用于内存记账，
/// 不是 data 处可读的长度；没返回包时不写。必须非 NULL</param>
/// <param name="status">输出：解包状态标志，见 prot_status</param>
/// <returns>解析完成的 redis_pack_ctx 链表头，数据不足或出错返回 NULL</returns>
void *redis_unpack(struct ev_ctx *ev, sock_ctx *sk, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);

#endif//REDIS_H_
