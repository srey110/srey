#ifndef PROTS_PUB_H_
#define PROTS_PUB_H_

#include "base/structs.h"

// 以下 10 个上限是可调的:某个判定在当前取值下恒假(如 16 位长度比 65535)不等于死代码,
// 上限调小它立刻生效,勿删
#define HTTP_MAX_HEADLENS (ONEK * 4) // HTTP 头部块 / trailer 块 / chunk 长度行的总长
#define HTTP_MAX_CONTENT_LENS 65535 // HTTP Content-Length 声明的 body 总长，整包一次缓冲
#define HTTP_MAX_CHUNK_LENS 65535 // HTTP 单个 chunk 的声明长度，每块一次分配；不限 body 总长
#define WS_MAX_PAYLOAD_LENS 65535 // WebSocket 单帧载荷
#define MQTT_MAX_PACK_LENS 65535 // MQTT 单条报文，含固定头
#define CUSTZ_MAX_PACK_LENS 65535 // 自定义协议数据体
#define SMTP_MAX_PACK_LENS 65535 // SMTP 单条响应(可多行)总字节，也管未见 CRLF 前的累积与 AUTH 挑战体
#define REDIS_MAX_LINE_LENS 65535 // Redis 长度行尚未收全时允许累积的字节数
#define REDIS_MAX_BULK_LENS (512 * 1024 * 1024) // Redis Bulk String，对齐 proto-max-bulk-len 默认值
#define MONGO_MAX_PACK_LENS (64 * 1024 * 1024) // MongoDB 单包，协议规范值

// mysql pgsql monogo smtp引用宏（ref：0=C 借用，事件层不释放块；>0=上层 handle 持有者数）
// 建连前 acquire：仅上层持有(ref>0)时 +1，C 借用(ref=0)短路
#define PROT_REF_ACQUIRE(ptr) \
    do { \
        if (0 != ATOMIC_GET(&(ptr)->ref)) { \
            ATOMIC_ADD(&(ptr)->ref, 1); \
        } \
    } while (0)
// release：C 借用(ref=0)短路，持有者归零时释放；事件层 udfree 与上层 handle 析构共用(析构时 ref 必>0，GET 短路恒真)。
// 密码/盐值的擦除必须落在这里(SECURE_FREE 整块)：析构侧逐字段擦不行——那时连接还活着，网络线程
// 可能正读着同一份密码组认证串；挪进 *_udfree 也不行——ctx 要活过连接，断线重连还要用它的密码
#define PROT_REF_RELEASE(ptr) \
    do { \
        if (0 != ATOMIC_GET(&(ptr)->ref) && 1 == ATOMIC_ADD(&(ptr)->ref, -1)) { \
            SECURE_FREE(ptr, sizeof(*(ptr))); \
        } \
    } while (0)

// 任务间消息类型枚举
typedef enum msg_type {
    MSG_TYPE_NONE = 0x00,   // 无消息（占位）
    MSG_TYPE_STARTUP,       // 任务启动
    MSG_TYPE_CLOSING,       // 任务关闭
    MSG_TYPE_TIMEOUT,       // 超时
    MSG_TYPE_ACCEPT,        // 新 TCP 连接接受
    MSG_TYPE_CONNECT,       // TCP 主动连接建立
    MSG_TYPE_SSLEXCHANGED,  // SSL 握手完成
    MSG_TYPE_HANDSHAKED,    // 应用层握手完成
    MSG_TYPE_RECV,          // TCP 数据接收
    MSG_TYPE_SEND,          // TCP 数据发送完成
    MSG_TYPE_CLOSE,         // 连接关闭
    MSG_TYPE_RECVFROM,      // UDP 数据接收
    MSG_TYPE_REQUEST,       // 任务间请求
    MSG_TYPE_RESPONSE,      // 任务间响应
    MSG_TYPE_FORK,          // 内部 mtype 标记：coro_fork/coro_fork_wait 的子任务经 fork_pending 链表，
                            // 在 dispatch 末尾 drain 起协程，_coro_mco_cb 据此路由到 _coro_fork_run（不入消息队列）
    MSG_TYPE_ALL            // 消息类型总数（边界值）
}msg_type;
// 协议包类型枚举
typedef enum pack_type {
    PACK_NONE = 0x00,       // 无协议（透传原始数据）
    PACK_DNS,               // DNS 协议
    PACK_HTTP,              // HTTP 协议
    PACK_WEBSOCK,           // WebSocket 协议
    PACK_MQTT,              // MQTT 协议
    PACK_SMTP,              // SMTP 协议
    PACK_CUSTZ_FIXED,       // 自定义协议 - 固定 4 字节长度头
    PACK_CUSTZ_FLAG,        // 自定义协议 - 标志位变长头
    PACK_CUSTZ_VAR,         // 自定义协议 - MQTT 风格变长头

    PACK_REDIS = 0x20,      // Redis RESP 协议
    PACK_MYSQL,             // MySQL 协议
    PACK_PGSQL,             // PostgreSQL 协议
    PACK_MONGO,             // MongoDB Wire 协议

    PACK_UDP_KCP = 0x40
}pack_type;
// 协议解包状态标志（可多个标志同时置位）
typedef enum prot_status {
    PROT_INIT = 0x00,          // 初始/正常状态
    PROT_SLICE_START = 0x01,   // 分片起始包
    PROT_SLICE = 0x02,         // 分片中间包
    PROT_SLICE_END = 0x04,     // 分片结束包
    PROT_ERROR = 0x08,         // 协议错误
    PROT_MOREDATA = 0x10,      // 数据不足，需等待更多数据
    PROT_CLOSE = 0x20          // 连接关闭信号
}prot_status;

// 任务间传递的消息体
typedef struct message_ctx {
    uint8_t slice;  // 分片类型（slice_type）
    uint8_t client; // 1 表示客户端连接，0 表示服务端连接
    subtype_t subtype; // 数据包解包类型（pack_type）或 请求类型（request_type）
    msg_type mtype;  // 消息类型
    int32_t erro;   // 错误码；CLOSE 上取 close_type（见 base/err.h）
    size_t size;    // 数据长度
    name_t src;     // 发送方任务名
    uint64_t sess;  // 会话 ID（用于请求/响应匹配）
    void *data;     // 消息数据指针
    shared_data *shared; // NULL=独占（默认 _message_clean 走 prots_pkfree/FREE）；非 NULL=task_multi_call / task_multi_request 广播,N 个 task 共享同一 data,各 task 释放时 ATOMIC_ADD(&ref,-1) 归 0 才 FREE
    sock_ctx sk;       // 连接标识
}message_ctx;
// 握手完成后的推送回调函数类型
typedef int32_t(*_handshaked_push)(sock_ctx *sk, int32_t client,
    ud_cxt *ud, int32_t erro, void *data, size_t lens);
// 消息汇：网络事件回调向上推消息的接口，由 task 层注册实现
typedef void*(*prots_emit_begin_cb)(void *loader, name_t handle);// 开窗：grab 目标，返回不透明句柄，NULL=目标不存在
typedef void(*prots_emit_cb)(void *target, message_ctx *msg);// 推一条消息给已开窗的目标
typedef void(*prots_emit_end_cb)(void *target);// 关窗：释放 begin 取得的句柄
typedef struct prot_emit {
    prots_emit_begin_cb begin;
    prots_emit_cb emit;
    prots_emit_end_cb end;
}prot_emit;
struct ev_ctx;

/// <summary>
/// 解析时间串里的小数秒 ".ffffff" 为微秒：从首个 '.' 起最多取 6 位，遇非数字即停，按补零对齐到 6 位。
/// mysql / pgsql 的 datetime 文本解析共用
/// </summary>
/// <param name="str">NUL 结尾的时间字符串</param>
/// <returns>微秒数 [0, 999999]；无小数点或小数点后无数字返回 0</returns>
uint32_t parse_usec_frac(const char *str);
/// <summary>
/// (指针, 长度) 的十进制浮点文本转 double，严格判定：整段必须被消费完、不接受空串、上溢即拒
/// ——三条都不是 strtod 自带的，上溢只有 errno 认得出来。
/// 下溢同样置 ERANGE 但返回的是正确的次正规数（DOUBLE 列的常规输出），放行。
/// strtod 直接认出的 "Infinity"/"-Infinity"/"NaN" 字面量是 PostgreSQL float 列的正常输出，
/// 不置 ERANGE 因而放行，由业务自行处置（test_pgsql_reader_double_bounds 锁了这条契约）。
/// mysql / pgsql 两侧的文本协议共用，别再各写一份
/// </summary>
/// <param name="data">源字节段(可非 NUL 结尾)</param>
/// <param name="lens">源字节数；0 视为失败</param>
/// <param name="val">输出：解析结果；返回 ERR_FAILED 时不写</param>
/// <returns>ERR_OK 成功；ERR_FAILED 空串/超 128 字节/有残留字符/上溢</returns>
int32_t parse_double_strict(const void *data, size_t lens, double *val);
/// <summary>
/// (指针, 长度) 的十进制整数文本转 int64，按符号拆开走 str2u64。
/// mysql / pgsql 两侧的文本协议共用，别再各写一份
/// </summary>
/// <param name="data">源字节段(可非 NUL 结尾)</param>
/// <param name="lens">源字节数；0 视为失败</param>
/// <param name="val">输出：解析结果；返回 ERR_FAILED 时不写</param>
/// <returns>ERR_OK 成功；空串/只有负号/含非数字字符/超出 int64 量程返回 ERR_FAILED</returns>
int32_t parse_int64_strict(const void *data, size_t lens, int64_t *val);
/// <summary>
/// 解析 "A[:B[:C]]" 形式的冒号分隔十进制三段值，各段按 max[i] 卡上界，缺的段填 0。
/// mysql 的 TIME 与 pgsql 的时区偏移共用，别再各写一份
/// </summary>
/// <param name="str">NUL 结尾的字符串，须从首段的数字起</param>
/// <param name="max">三段各自的上界(含)</param>
/// <param name="val">输出：三段值，未出现的段写 0；返回 0 时三段都不写</param>
/// <returns>成功解析的段数 1~3；首段就不是数字、或任一段超上界返回 0</returns>
int32_t parse_colon_triple(const char *str, const uint32_t max[3], uint32_t val[3]);

#endif// PROTS_PUB_H_
