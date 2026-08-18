#ifndef PROTS_PUB_H_
#define PROTS_PUB_H_

#include "base/structs.h"

// mysql pgsql monogo smtp引用宏（ref：0=C 借用，事件层不释放块；>0=上层 handle 持有者数）
// 建连前 acquire：仅上层持有(ref>0)时 +1，C 借用(ref=0)短路
#define PROT_REF_ACQUIRE(ptr) \
    do { \
        if (0 != ATOMIC_GET(&(ptr)->ref)) { \
            ATOMIC_ADD(&(ptr)->ref, 1); \
        } \
    } while (0)
// release：C 借用(ref=0)短路，持有者归零时释放；事件层 udfree 与上层 handle 析构共用(析构时 ref 必>0，GET 短路恒真)。
// 释放走 SECURE_FREE 整块擦除，四种 ctx 里的密码/盐值都在这一刻抹掉——而不是在析构时逐字段擦：
// 析构侧的 ev_close 只是往网络线程投一条命令，返回时连接还活着，网络线程可能正读着同一个密码
// 组认证串（如 smtp 的 _smtp_loin_cmd），工作线程当场 secure_zero 就是一对无同步的读写，
// 现场表现是把清了一半的密码发出去。挪到这里则天然没有竞争：ATOMIC_ADD 返回旧值，看到 1 的
// 那个线程是最后一个持有者，其余都已放手（网络线程那份在 *_udfree 里释放，释放前已 ud->context=NULL），
// 那次原子 RMW 本身就是同步点。代价是明文多驻留一个命令往返。
// 也不能改在 *_udfree 里擦：ctx 会活过连接，断线重连还要拿它的密码
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
    int32_t erro;   // 错误码
    size_t size;    // 数据长度
    name_t src;     // 发送方任务名
    uint64_t sess;  // 会话 ID（用于请求/响应匹配）
    void *data;     // 消息数据指针
    shared_data *shared; // NULL=独占（默认 _message_clean 走 prots_pkfree/FREE）；非 NULL=task_multi_call / task_multi_request 广播,N 个 task 共享同一 data,各 task 释放时 ATOMIC_ADD(&ref,-1) 归 0 才 FREE
    sk_id sk;       // 连接标识 fd+skid
}message_ctx;
// 握手完成后的推送回调函数类型
typedef int32_t(*_handshaked_push)(SOCKET fd, uint64_t skid, int32_t client,
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
/// mysql / pgsql 两侧的文本协议共用，别再各写一份
/// </summary>
/// <param name="data">源字节段(可非 NUL 结尾)</param>
/// <param name="lens">源字节数；0 视为失败</param>
/// <param name="val">输出：解析结果；返回 ERR_FAILED 时不写</param>
/// <returns>ERR_OK 成功；ERR_FAILED 空串/超 128 字节/有残留字符/上溢</returns>
int32_t parse_double_strict(const void *data, size_t lens, double *val);

#endif// PROTS_PUB_H_
