#ifndef STRUCTS_H_
#define STRUCTS_H_

#include "base/macro.h"

typedef uint32_t name_t; // 任务名类型（32 位整数 ID）

// 连接用户自定义上下文，挂载在每个网络连接上
typedef struct ud_cxt {
    uint8_t  pktype;  // 数据包类型（由上层协议定义）
    uint8_t  status;  // 解包状态（协议解析状态机）
    name_t   name;    // 所属任务名（task 的标识 ID）
    void    *loader;  // 指向 loader_ctx，用于访问 loader
    void    *context; // 上层上下文指针（超时回调时使用）
    uint64_t sess;    // 会话 ID（用于超时匹配）
}ud_cxt;

// 通用数据缓冲区（仅持有指针，不管理内存所有权）
typedef struct buf_ctx {
    void   *data; // 数据指针
    size_t  lens; // 数据长度（字节）
}buf_ctx;

// 带偏移量的数据缓冲区（用于流式读取）
typedef struct off_buf_ctx {
    void   *data;   // 数据指针
    size_t  lens;   // 数据总长度（字节）
    size_t  offset; // 当前读取偏移量
}off_buf_ctx;
struct task_ctx;

typedef void(*free_cb)(void *arg); // 通用资源释放回调函数类型

// 将 src 的内容复制到 dst；src 为 NULL 时将 dst 清零
#define COPY_UD(dst, src)\
    if (NULL != (src)){\
        (dst) = *(src);\
    }else{\
        ZERO(&(dst), sizeof(ud_cxt));\
    }

// 判断 buf_ctx 是否为空（指针或数据为 NULL 或长度为 0）
static inline int32_t buf_empty(buf_ctx *buf) {
    return NULL == buf || NULL == buf->data || 0 == buf->lens;
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
