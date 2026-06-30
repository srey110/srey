#ifndef STRUCTS_H_
#define STRUCTS_H_

#include "base/macro.h"

typedef uint16_t subtype_t; //子类型
typedef uint64_t name_t; // 任务名类型（64 位整数 ID）

// 连接用户自定义上下文，挂载在每个网络连接上
typedef struct ud_cxt {
    uint8_t  status;  // 解包状态（协议解析状态机）
    subtype_t pktype;  // 数据包类型（由上层协议定义）
    size_t   prot_offset; // 协议解析私有偏移（如 HTTP 头扫描位置缓存），由各协议自行管理
    name_t   handle;  // 所属任务句柄（task 的标识 ID）
    uint64_t sess;    // 会话 ID（用于超时匹配）
    void    *loader;  // 指向 loader_ctx，用于访问 loader
    void    *context; // 上层上下文指针（超时回调时使用）
}ud_cxt;
// socket 标识：fd 定位 socket，skid 防 fd 复用，两者合起来确定一条活连接
typedef struct sk_id {
    SOCKET   fd;    // socket 句柄
    uint64_t skid;  // 连接 ID（防 fd 复用误操作）
}sk_id;
// 通用数据缓冲区（仅持有指针，不管理内存所有权）
typedef struct buf_ctx {
    size_t  lens; // 数据长度（字节）
    void   *data; // 数据指针
}buf_ctx;
// 多播共享 pack：ev_send_multi 一次广播给 N 个 fd 时,N 个 off_buf_ctx.shared 指向同一份;
// 每个 buf 释放走 _evpub_off_buf_release → ATOMIC_ADD(&ref,-1),归 0 时 FREE(data)+FREE(pack)
typedef struct shared_data {
    atomic_t ref;   // 剩余未释放 buf 数；初始 = ev_send_multi 有效 fd 数
    void *data;     // 业务负载,所有 buf 引用同一份
}shared_data;
// 带偏移量的数据缓冲区（用于流式读取）
typedef struct off_buf_ctx {
    size_t  lens;   // 数据总长度（字节）
    size_t  offset; // 当前读取偏移量
    void   *data;   // 数据指针
    shared_data *shared; // NULL=独占（默认 FREE(data)）；非 NULL=多播共享（ev_send_multi 投递），buf 释放时 ATOMIC_ADD(&shared->ref,-1) 归 0 才 FREE pack->data + pack 自身
}off_buf_ctx;

typedef void(*free_cb)(void *arg); // 通用资源释放回调函数类型

// 将 src 的内容复制到 dst；src 为 NULL 时将 dst 清零
#define COPY_UD(dst, src)\
    do {\
        if (NULL != (src)){\
            (dst) = *(src);\
        }else{\
            ZERO(&(dst), sizeof(ud_cxt));\
        }\
    } while(0)
// 调用用户数据释放回调（ud_free 不为 NULL 时才调用）
#define UD_FREE(ud_free, ud) \
    do { \
        if (NULL != (ud_free)) { \
            (ud_free)(ud); \
        } \
    } while (0)

// 判断 buf_ctx 是否为空（指针或数据为 NULL 或长度为 0）
static inline int32_t buf_empty(buf_ctx *buf) {
    return NULL == buf || EMPTYPTR(buf->data, buf->lens);
};
// 区分大小写地比较 buf_ctx 与指定数据是否相等
static inline int32_t buf_compare(buf_ctx *buf, const char *data, size_t lens) {
    return buf->lens == lens && 0 == memcmp(buf->data, data, lens);
};
// 不区分大小写地比较 buf_ctx 与指定数据是否相等
static inline int32_t buf_icompare(buf_ctx *buf, const char *data, size_t lens) {
    return buf->lens == lens && 0 == _memicmp(buf->data, data, lens);
};

#endif//STRUCTS_H_
