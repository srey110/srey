#ifndef STRUCTS_H_
#define STRUCTS_H_

#include "base/base.h"

typedef uint16_t subtype_t; //子类型
typedef uint64_t name_t; // 任务名类型（64 位整数 ID）
typedef void(*free_cb)(void *arg); // 通用资源释放回调函数类型

// ud_cxt.flags 的位，按位组合；accept 出的连接从监听继承
typedef enum ud_flag {
    UD_PEERADDR = 0x01, // ACCEPT 消息带对端地址
    UD_REUSEADDR = 0x02, // UDP 绑定前设 SO_REUSEADDR，允许多个 socket 共用 addr:port(多播要)；不设则独占
    UD_SESS = 0x04 // 监听用：accept 出的连接会话键预设为它自己的 skid(同 ev_connect 的 setsess)
}ud_flag;
// 连接用户自定义上下文，挂载在每个网络连接上
typedef struct ud_cxt {
    uint8_t  status;  // 解包状态（协议解析状态机）
    uint8_t  flags;   // 连接级开关位，见 ud_flag；占的是 status 与 pktype 之间的空档，不增大结构体
    subtype_t pktype;  // 数据包类型（由上层协议定义）
    size_t   prot_offset; // 协议解析私有偏移（如 HTTP 头扫描位置缓存），由各协议自行管理
    name_t   handle;  // 所属任务句柄（task 的标识 ID）
    uint64_t sess;    // 会话 ID（用于超时匹配）
    void    *loader;  // 指向 loader_ctx，用于访问 loader
    void    *context; // 上层上下文指针（超时回调时使用）
}ud_cxt;
// socket 标识：fd 定位 socket，skid 防 fd 复用，index 记所属 watcher，三者合起来确定一条活连接。
// index 一般等于 CALC_WATCHER_INDEX(fd, nthreads);listener 与 pipe 按所在 watcher 的下标填,失效的填 INVALID_INDEX。
// 字段顺序不能动：挪到末尾 unix 下 sizeof 从 16 涨到 24;挪到开头会让 { INVALID_SOCK, INVALID_INDEX, 0 }
// 这类位置初始化整体错位,Windows 下 fd 拿到的是 -1 而非 INVALID_SOCKET,sock_is_invalid 会漏判
typedef struct sock_ctx {
    SOCKET   fd;    // socket 句柄
    int32_t  index; // 所属 watcher 在 ctx->watcher 中的下标,取值 [0, nthreads) 或 INVALID_INDEX
    uint64_t skid;  // 连接 ID（防 fd 复用误操作）
}sock_ctx;
// 广播共享载荷：多个持有者共用一份 data,shared_data_free 减到 0 时释放 data 与自身。
// ev_send_multi 的父计数与组内计数、task_multi_call 的消息都用它
typedef struct shared_data {
    atomic_t ref;   // 剩余持有者数
    void *data;     // 共用的那份数据(组内计数里是父计数)
}shared_data;
// 带偏移量的数据缓冲区（用于流式读取）
typedef struct off_buf_ctx {
    size_t  lens;   // 数据总长度（字节）
    size_t  offset; // 当前读取偏移量
    void   *data;   // 数据指针
    shared_data *shared; // NULL=独占（释放即 FREE(data)）；非 NULL=ev_send_multi 的组内计数，其 data 指向父计数，释放见 _evpub_off_buf_release
}off_buf_ctx;

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
        if (NULL != (ud_free) && NULL != (ud)) { \
            (ud_free)(ud); \
        } \
    } while (0)

/// <summary>
/// 连接标识是否失效。只看 fd：skid 要留着判"中途是否重连过"
/// </summary>
/// <param name="sk">连接标识</param>
/// <returns>非 0 已失效（fd 为 INVALID_SOCK），0 有效</returns>
static inline int32_t sock_is_invalid(sock_ctx *sk) {
    return INVALID_SOCK == sk->fd;
}
/// <summary>
/// 网络线程关连接时置失效用，只写 fd，index 与 skid 都不动：别的线程可能正经 ev_* 先判 fd 再取 index，
/// index 留旧值才不会读到"fd 有效、index 为 -1"的半截而撞断言，投到原 watcher 的命令按 skid 丢弃
/// </summary>
/// <param name="sk">连接标识</param>
static inline void sock_mark_closed(sock_ctx *sk) {
    sk->fd = INVALID_SOCK;
}
/// <summary>
/// 把连接标识置为失效。不动 skid，理由同 sock_is_invalid；index 置 INVALID_INDEX：
/// 0 是合法下标，表达不了"未设置"，拿它去索引会静默投到 watcher[0] 上
/// </summary>
/// <param name="sk">连接标识</param>
static inline void sock_set_invalid(sock_ctx *sk) {
    sock_mark_closed(sk);
    sk->index = INVALID_INDEX;
}
/// <summary>
/// 一个持有者放手：ref 减 1，减到 0 的那一个调 fcb(data) 并释放 shared_data 自身。每个持有者调且只调一次
/// </summary>
/// <param name="sdata">shared_data 指针，须经 MALLOC 分配（减到 0 时 FREE）</param>
/// <param name="fcb">释放 data 的回调；NULL 表示 data 不用释放</param>
static inline void shared_data_free(void *sdata, free_cb fcb) {
    shared_data *sd = sdata;
    if (1 == ATOMIC_ADD(&sd->ref, -1)) {
        if (NULL != fcb) {
            fcb(sd->data);
        }
        FREE(sd);
    }
}

#endif//STRUCTS_H_
