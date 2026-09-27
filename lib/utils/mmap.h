#ifndef MMAP_H_
#define MMAP_H_

#include "base/macro.h"

// 通用内存映射：文件、匿名、命名共享内存三种来源，统一成一个 mmap_ctx。
// 契约（各函数的注释引用这里的编号）：
// 1. 只保证"在 cap 内变长/变短"时 addr 不变；超出 cap 的 resize 可能换地址，旧指针全部失效，
//    改过的保护属性与锁页也随之失效。
// 2. [size, cap) 这段不可访问：文件映射在 POSIX 上是 SIGBUS，Windows 上是访问违例。
// 3. 映射期间文件被别人截短、或底层 I/O 出错，访问对应页会让进程崩溃（SIGBUS / EXCEPTION_IN_PAGE_ERROR），不捕获。
// 4. 读写文件映射的文件长度：映射期间只增不减，close 时定为 max(映射前长度, off + size)，
//    只截回本映射自己扩出来的部分（别人把文件写得更长时不动）。
//    Windows 上预留容量（cap > size）期间文件长度按页向上取整，close 时才截回精确值；
//    别的进程还映射着该文件时 Windows 截不回去，文件保持原长。
// 5. 往映射里写不等于落盘，要持久化调 mmap_sync。
// 6. 除"在 cap 内变长"可以与其他线程读已有部分并发外，其余操作都要调用方保证不同时发生。
// 7. 写时复制映射里没写过的页，能否看到别人之后写进文件的内容各平台不同（Linux/FreeBSD 能，macOS 不能）；
//    Windows 在建映射时就按整段长度扣提交额度，文件很大时可能直接失败。
// 8. Windows 上映射与 ReadFile / WriteFile 混用不保证一致，同一文件别两种方式一起写。

typedef enum mmap_mode {
    MMAP_RDONLY = 0x00, // 只读共享
    MMAP_RDWR,          // 读写共享：写入进文件，其他进程可见
    MMAP_COPY,          // 写时复制：可写，但改动只在本进程、不进文件
}mmap_mode;
typedef enum mmap_flag {
    MMAP_CREATE = 0x01,   // 不存在就创建（mmap_open / mmap_shm）
    MMAP_EXCL = 0x02,     // 与 MMAP_CREATE 同用：已存在就失败
    MMAP_POPULATE = 0x04, // 映射完立即读进内存（只是提示）
}mmap_flag;
typedef enum mmap_advice {
    MMAP_NORMAL = 0x00, // 恢复默认的预读策略
    MMAP_SEQUENTIAL,    // 将顺序访问：多预读，读过的页可以早点换出
    MMAP_RANDOM,        // 将随机访问：关预读，数据比内存大时用
    MMAP_WILLNEED,      // 马上要用：提前读进内存
    MMAP_DONTNEED,      // 暂时不用：可以先换出，共享映射的数据不丢（私有映射拒绝，见 mmap_advise）
}mmap_advice;
#ifdef OS_WIN
typedef HANDLE mmap_fd;
#else
typedef int32_t mmap_fd;
#endif
// 创建参数，整体清零即默认：只读、从头映射到文件末尾、不预留
typedef struct mmap_opts {
    int32_t mode;  // mmap_mode
    int32_t flags; // mmap_flag 组合
    uint32_t perm; // 创建时的权限位，0 表示文件 0644、共享内存 0600；Windows 忽略
    uint64_t off;  // 文件偏移，任意值，内部按 mmap_granularity 向下对齐
    size_t size;   // 映射长度，0 表示到文件末尾
    size_t cap;    // 预留长度，0 或小于 size 时取 size；只有读写文件映射与匿名映射可以大于 size
}mmap_opts;
typedef struct mmap_ctx {
    int32_t kind;   // 来源，0 表示未建或已关闭
    int32_t mode;   // mmap_mode
    int32_t flags;  // mmap_flag
    mmap_fd fd;     // 自己持有的句柄：文件；Windows 命名共享内存存的是区段句柄（名字靠句柄活着）；匿名映射没有
    size_t delta;   // addr 相对真实映射起点的偏移
    size_t size;    // 可访问长度
    size_t cap;     // 已映射（预留）长度，size <= cap
    uint64_t off;   // 文件偏移
    uint64_t flen0; // close 截回的下限：映射前长度，扩文件前发现别人已写长就抬到那个长度
    uint64_t hw;    // 本映射把文件设到过的最大长度
    char *addr;     // 映射起点；从没建过映射时为 NULL
}mmap_ctx;

/// <summary>
/// 相对 addr 的 [off, off + lens) 是否落在可访问范围内，直接读写 addr 前用
/// </summary>
/// <param name="ctx">mmap_ctx</param>
/// <param name="off">相对 addr 的起点</param>
/// <param name="lens">长度</param>
/// <returns>1 在范围内；0 越界，addr 为 NULL（没建映射或 resize 失败）时非空区间也算越界</returns>
static inline int32_t mmap_in_range(const mmap_ctx *ctx, size_t off, size_t lens) {
    return off <= ctx->size && lens <= ctx->size - off && (0 == lens || NULL != ctx->addr);
}
/// <summary>
/// 取映射里 [off, off + lens) 的起点，越界判断同 mmap_in_range
/// </summary>
/// <param name="ctx">mmap_ctx</param>
/// <param name="off">相对 addr 的起点</param>
/// <param name="lens">要访问的长度</param>
/// <returns>addr + off；越界或没建映射时 NULL。只到下次 resize / close 前有效（契约 1）</returns>
static inline void *mmap_ptr(const mmap_ctx *ctx, size_t off, size_t lens) {
    return off <= ctx->size && lens <= ctx->size - off && NULL != ctx->addr ? ctx->addr + off : NULL;
}
/// <summary>
/// 可访问长度
/// </summary>
/// <param name="ctx">mmap_ctx</param>
/// <returns>size</returns>
static inline size_t mmap_size(const mmap_ctx *ctx) {
    return ctx->size;
}
/// <summary>
/// 按路径打开文件并映射。路径编码与项目其他文件接口一致：Windows 上按系统代码页（ANSI）解释
/// </summary>
/// <param name="ctx">mmap_ctx，函数内先整体初始化；失败后仍可安全调 mmap_close</param>
/// <param name="path">文件路径</param>
/// <param name="opts">创建参数，NULL 等同全零。RDONLY / COPY 下 off + size 不能超过文件长度；
/// RDWR 下文件不够长就扩到 off + size。size 最终为 0 时不建映射（addr 为 NULL），第一次 resize 变大时才建</param>
/// <returns>ERR_OK 成功；ERR_FAILED 失败，原因看 ERRNO</returns>
int32_t mmap_open(mmap_ctx *ctx, const char *path, const mmap_opts *opts);
/// <summary>
/// 映射调用方已打开的文件。内部复制一份句柄自己持有，调用方的句柄随时可关
/// </summary>
/// <param name="ctx">mmap_ctx，同 mmap_open</param>
/// <param name="fd">已打开的文件；访问权限须覆盖 opts->mode（RDWR 要可写）</param>
/// <param name="opts">同 mmap_open，其中 MMAP_CREATE / MMAP_EXCL / perm 不起作用</param>
/// <returns>同 mmap_open</returns>
int32_t mmap_map_fd(mmap_ctx *ctx, mmap_fd fd, const mmap_opts *opts);
/// <summary>
/// 匿名映射（进程私有、内容初始为 0）。预留 cap，只提交前 size 字节
/// </summary>
/// <param name="ctx">mmap_ctx，同 mmap_open</param>
/// <param name="opts">只用 size / cap / flags(MMAP_POPULATE)；mode 固定为 RDWR</param>
/// <returns>同 mmap_open</returns>
int32_t mmap_anon(mmap_ctx *ctx, const mmap_opts *opts);
/// <summary>
/// 命名共享内存。长度在创建时定死，不支持 resize；生命周期两个平台不同：
/// POSIX 一直存在到 mmap_shm_unlink（或重启），Windows 最后一个打开它的 ctx 关掉就消失
/// </summary>
/// <param name="ctx">mmap_ctx，同 mmap_open</param>
/// <param name="name">"/名字" 形式，名字里不能再有 '/'，总长 2~31 字节（macOS 的上限，各平台统一按它拦）。
/// Windows 上放进 Local\ 命名空间</param>
/// <param name="opts">mode 只能是 RDONLY / RDWR（macOS 的 POSIX 共享内存不支持私有映射）；创建时 size 必须 > 0；
/// 打开已有对象时 size 为 0 表示整个对象，大于现有长度则失败；cap 须为 0 或等于 size</param>
/// <returns>同 mmap_open；打开已有对象时 size 可能是按页向上取整后的值（macOS 与 Windows）</returns>
int32_t mmap_shm(mmap_ctx *ctx, const char *name, const mmap_opts *opts);
/// <summary>
/// 删除命名共享内存的名字；已映射的仍可用。Windows 上什么也不做
/// </summary>
/// <param name="name">同 mmap_shm</param>
/// <returns>ERR_OK 成功，Windows 上名字合法即返回它；ERR_FAILED 名字不合法或删除失败</returns>
int32_t mmap_shm_unlink(const char *name);
/// <summary>
/// 解除映射并关闭句柄；读写文件映射按契约 4 定文件长度。重复调用、对全零的 ctx 调用都安全
/// </summary>
/// <param name="ctx">mmap_ctx</param>
void mmap_close(mmap_ctx *ctx);
/// <summary>
/// 改映射长度。cap 内变长/变短 addr 不变；超出 cap 按新 cap 重映射，地址可能变（契约 1）。
/// 读写文件映射文件不够长就扩（契约 4）；只读 / 写时复制映射变长到超出原映射时重映射，用来跟上别人写长的文件，
/// 写时复制映射原有的私有改动保留。
/// 匿名映射变短时把多出的内存还给系统，再变长时那部分重新为 0。命名共享内存不支持
/// </summary>
/// <param name="ctx">mmap_ctx</param>
/// <param name="size">新长度</param>
/// <param name="cap">新预留长度，只增不减：不大于当前 cap 时保持原值，小于 size 时取 size；
/// 只读 / 写时复制映射不能预留（须为 0 或不大于 size）</param>
/// <returns>ERR_OK 成功；ERR_FAILED 参数越界、命名共享内存、只读 / 写时复制映射越过文件末尾或系统调用失败，
/// 此时 addr 可能已变成 NULL（未映射），可再调 resize 重试或 close</returns>
int32_t mmap_resize(mmap_ctx *ctx, size_t size, size_t cap);
/// <summary>
/// 刷盘。只对读写文件映射起作用，其余映射什么也不做。
/// 同步刷盘在 macOS 上用 F_FULLFSYNC，Windows 上 FlushViewOfFile 之后再 FlushFileBuffers
/// </summary>
/// <param name="ctx">mmap_ctx</param>
/// <param name="off">相对 addr 的起点</param>
/// <param name="lens">长度，0 表示到 size 末尾；off + lens 不能超过 size</param>
/// <param name="async">1 只交给系统不等写完</param>
/// <returns>ERR_OK 成功，不是读写文件映射时也返回它；ERR_FAILED 越界、ctx 未映射或刷盘失败</returns>
int32_t mmap_sync(mmap_ctx *ctx, size_t off, size_t lens, int32_t async);
/// <summary>
/// 访问模式建议。写时复制与匿名映射不收 MMAP_DONTNEED（Linux 会丢改动，别的平台不会）；
/// Windows 上只有 MMAP_WILLNEED 有效
/// </summary>
/// <param name="ctx">mmap_ctx</param>
/// <param name="off">相对 addr 的起点</param>
/// <param name="lens">长度，0 表示到 size 末尾；off + lens 不能超过 size</param>
/// <param name="advice">mmap_advice</param>
/// <returns>ERR_OK 成功，Windows 上 MMAP_WILLNEED 以外的建议也返回它；
/// ERR_FAILED advice 非法、写时复制或匿名映射给了 MMAP_DONTNEED、越界、ctx 未映射或系统调用失败</returns>
int32_t mmap_advise(mmap_ctx *ctx, size_t off, size_t lens, int32_t advice);
/// <summary>
/// 改保护属性，不能超出建映射时的权限；写时复制映射改回可写后改动仍只在本进程
/// </summary>
/// <param name="ctx">mmap_ctx</param>
/// <param name="off">相对 addr 的起点</param>
/// <param name="lens">长度，0 表示到 size 末尾；off + lens 不能超过 size</param>
/// <param name="mode">MMAP_RDONLY 或 MMAP_RDWR</param>
/// <returns>ERR_OK 成功；ERR_FAILED mode 非法、只读映射要改可写、越界、ctx 未映射或系统调用失败</returns>
int32_t mmap_protect(mmap_ctx *ctx, size_t off, size_t lens, int32_t mode);
/// <summary>
/// 锁页，不让换出。受 POSIX 的 RLIMIT_MEMLOCK 与 Windows 进程工作集下限约束（Windows 默认只有约 200KB）
/// </summary>
/// <param name="ctx">mmap_ctx</param>
/// <param name="off">相对 addr 的起点</param>
/// <param name="lens">长度，0 表示到 size 末尾；off + lens 不能超过 size</param>
/// <returns>ERR_OK 成功；ERR_FAILED 失败</returns>
int32_t mmap_lock(mmap_ctx *ctx, size_t off, size_t lens);
/// <summary>
/// 解除锁页
/// </summary>
/// <param name="ctx">mmap_ctx</param>
/// <param name="off">同 mmap_lock</param>
/// <param name="lens">同 mmap_lock</param>
/// <returns>ERR_OK 成功；ERR_FAILED 失败</returns>
int32_t mmap_unlock(mmap_ctx *ctx, size_t off, size_t lens);
/// <summary>
/// 文件当前长度，用来跟上别的进程写长的文件（配合 mmap_resize）
/// </summary>
/// <param name="ctx">mmap_ctx</param>
/// <returns>文件长度；不是文件映射或失败时 ERR_FAILED</returns>
int64_t mmap_filesize(mmap_ctx *ctx);
/// <summary>
/// 文件偏移的对齐粒度：Windows 64KB，POSIX 为页大小。滑动窗口读大文件时按它选偏移可以不白映射
/// </summary>
/// <returns>粒度（字节）</returns>
size_t mmap_granularity(void);

#endif//MMAP_H_
