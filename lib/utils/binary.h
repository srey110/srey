#ifndef BINARY_H_
#define BINARY_H_

#include "base/base.h"
#include "base/structs.h"
#include "utils/utils.h"

typedef struct binary_ctx {
    char *data;
    size_t inc;//增加基数
    size_t size;//总长度
    size_t offset;//数据长度
}binary_ctx;

/// <summary>
/// 写模式初始化：自己 malloc 一块可扩容的缓冲，binary_set_* 与 binary_get_* 都能用。
/// 用完须 binary_free。组包走这个，解包走 binary_init_read。
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="lens">初始容量提示；0 表示只按 inc 开一块</param>
/// <param name="inc">扩容增量基数，取值 [0, INT32_MAX]，超界断言。内部会向上取到 2 的幂
/// （下限 2）——它随后当对齐模数用，非 2 的幂对不齐；0 表示用默认值</param>
void binary_init_write(binary_ctx *ctx, size_t lens, size_t inc);
/// <summary>
/// 读模式初始化：直接包住调用方给的内存，不分配、不扩容，binary_free 也不会释放它。
/// 只能用 binary_get_* / binary_offset / binary_at 这些不扩容的接口，
/// 调用 binary_set_* / binary_set_va 会 ASSERT 失败。
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="buf">外部缓冲区，须非 NULL 且在 ctx 用完前一直有效；所有权不转移</param>
/// <param name="lens">buf 的可读长度</param>
void binary_init_read(binary_ctx *ctx, char *buf, size_t lens);
/// <summary>
/// 释放写模式自己分配的缓冲；读模式（inc==0）包着的外部内存不动。
/// 调用后 data=NULL、size=offset=0，可安全重复调用。
/// </summary>
/// <param name="ctx">binary_ctx</param>
void binary_free(binary_ctx *ctx);
// 扩展缓冲区，确保有足够空间写入 size 字节（仅内部托管可扩容）
static inline void _binary_expand(binary_ctx *ctx, size_t size) {
    //inc==0 标记外部托管 buf：不接管所有权，任何 binary_set_* 都会从 offset 起改写调用方内存，
    //超出容量时还要对栈/静态/异分配器内存调 REALLOC(UB)。守卫必须在容量判断之前——
    //放在 if 内只挡得住写溢出的那次，写得下的同样非法却会静默损坏调用方数据
    ASSERTAB(0 != ctx->inc, "read-mode buffer is read-only: use binary_init_write for writable mode");
    ASSERTAB(size <= SIZE_MAX - ctx->offset - 1, "binary buffer size overflow");
    size += ctx->offset + 1;
    if (size > ctx->size) {
        size_t lens = ctx->size * 2;
        if (lens < size) {
            lens = size;
        }
        ctx->size = ROUND_UP(lens, ctx->inc);
        // 翻倍与取整都可能溢出回绕成 0，而 _realloc(0) 按契约释放并返回 NULL，下面就 memmove 到空指针
        ASSERTAB(0 != ctx->size, "binary buffer size overflow");
        REALLOC(ctx->data, ctx->data, ctx->size);
    }
}
// 追加 lens 字节到末尾。reserve 是本次除 lens 外还要一次扩够的容量,给 binary_set_string 的结尾 NUL 用,
// 分两次扩容会在容量刚好卡住时多 REALLOC 一趟。
// buf 允许指向 ctx 自己的缓冲:扩容的 REALLOC 会把它搬走,所以先换算成下标,拷贝也随之改走 memmove。
// 别把内外判定改成指针关系比较:两者多数时候不属同一对象,那样是 UB,优化后会被折成恒假
static inline void _binary_append(binary_ctx *ctx, const char *buf, size_t lens, size_t reserve) {
    uintptr_t aoff = (uintptr_t)buf - (uintptr_t)ctx->data;
    int32_t inner = (NULL != ctx->data && aoff < ctx->size);
    _binary_expand(ctx, lens + reserve);
    memmove(ctx->data + ctx->offset, inner ? ctx->data + aoff : buf, lens);
    ctx->offset += lens;
}
/// <summary>
/// 设置偏移值
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="off">偏移</param>
static inline void binary_offset(binary_ctx *ctx, size_t off) {
    ASSERTAB(off <= ctx->size, "binary offset out of bounds");
    ctx->offset = off;
}
/// <summary>
/// 写入int8
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="val">值</param>
static inline void binary_set_int8(binary_ctx *ctx, int8_t val) {
    _binary_expand(ctx, sizeof(val));
    (ctx->data + ctx->offset)[0] = val;
    ctx->offset += sizeof(val);
}
/// <summary>
/// 写入uint8
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="val">值</param>
static inline void binary_set_uint8(binary_ctx *ctx, uint8_t val) {
    _binary_expand(ctx, sizeof(val));
    (ctx->data + ctx->offset)[0] = (int8_t)val;
    ctx->offset += sizeof(val);
}
/// <summary>
/// 写入整数
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="val">值</param>
/// <param name="lens">val字节数</param>
/// <param name="islittle">1 小端序列 0大端序列</param>
static inline void binary_set_integer(binary_ctx *ctx, int64_t val, size_t lens, int32_t islittle) {
    _binary_expand(ctx, lens);
    pack_integer(ctx->data + ctx->offset, (uint64_t)val, (int32_t)lens, islittle);
    ctx->offset += lens;
}
/// <summary>
/// 写入无符号整数
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="val">值</param>
/// <param name="lens">val字节数</param>
/// <param name="islittle">1 小端序列 0大端序列</param>
static inline void binary_set_uinteger(binary_ctx *ctx, uint64_t val, size_t lens, int32_t islittle) {
    _binary_expand(ctx, lens);
    pack_integer(ctx->data + ctx->offset, val, (int32_t)lens, islittle);
    ctx->offset += lens;
}
/// <summary>
/// 写入float
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="val">值</param>
/// <param name="islittle">1 小端序列 0大端序列</param>
static inline void binary_set_float(binary_ctx *ctx, float val, int32_t islittle) {
    _binary_expand(ctx, sizeof(val));
    pack_float(ctx->data + ctx->offset, val, islittle);
    ctx->offset += sizeof(val);
}
/// <summary>
/// 写入double
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="val">值</param>
/// <param name="islittle">1 小端序列 0大端序列</param>
static inline void binary_set_double(binary_ctx *ctx, double val, int32_t islittle) {
    _binary_expand(ctx, sizeof(val));
    pack_double(ctx->data + ctx->offset, val, islittle);
    ctx->offset += sizeof(val);
}
/// <summary>
/// 写入以'\0'结束的字符串。buf 允许指向本 ctx 自己的缓冲，规则同 binary_set_binary
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="buf">值</param>
static inline void binary_set_string(binary_ctx *ctx, const char *buf) {
    if (NULL == buf) {
        return;
    }
    _binary_append(ctx, buf, strlen(buf), 1);
    ctx->data[ctx->offset] = '\0';
    ctx->offset++;
}
/// <summary>
/// 写入char *。buf 允许指向本 ctx 自己的缓冲：扩容搬走后会自动换算成新地址，重叠也按 memmove 处理
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="buf">值</param>
/// <param name="lens">字节数</param>
static inline void binary_set_binary(binary_ctx *ctx, const char *buf, size_t lens) {
    if (NULL == buf || 0 == lens) {
        return;
    }
    _binary_append(ctx, buf, lens, 0);
}
/// <summary>
/// 填充
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="val">以该值填充</param>
/// <param name="lens">填充长度</param>
static inline void binary_set_fill(binary_ctx *ctx, char val, size_t lens) {
    _binary_expand(ctx, lens);
    memset(ctx->data + ctx->offset, val, lens);
    ctx->offset += lens;
}
/// <summary>
/// 跳过指定长度
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="lens">长度</param>
static inline void binary_set_skip(binary_ctx *ctx, size_t lens) {
    _binary_expand(ctx, lens);
    ctx->offset += lens;
}
/// <summary>
/// 写入变参数据，按需扩容。外部托管的缓冲不支持，传进来即断言
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="fmt">格式化</param>
/// <param name="...">变参</param>
void binary_set_va(binary_ctx *ctx, const char *fmt, ...);
// 全部 binary_get_* 越界即 ASSERTAB abort 进程，没有失败回传通道。凡是长度由对端决定的
// 报文，读之前必须先用下面两个判一遍——判定与读挨着写，漏判在 review 里看得见。
// binary_get_string 是唯一预判不了的（越界条件是"剩余字节里没有 NUL"，得先扫），
// 那种场合改用 binary_try_get_string。
/// <summary>
/// 还剩多少字节没读
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <returns>剩余字节数</returns>
static inline size_t binary_remain(binary_ctx *ctx) {
    return ctx->size - ctx->offset;
}
/// <summary>
/// 还够不够读 lens 字节。lens 收 uint64_t 不收 size_t：报文里的长度字段常是 64 位，
/// 调用方若先转窄再比，32 位构建上 0x1_0000_0001 会截成 1 蒙混过关
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="lens">打算读的字节数</param>
/// <returns>够返回非 0</returns>
static inline int32_t binary_have(binary_ctx *ctx, uint64_t lens) {
    return lens <= (uint64_t)(ctx->size - ctx->offset);
}
/// <summary>
/// 获取指定位置的指针
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="pos">位置</param>
/// <returns>char *</returns>
static inline char *binary_at(binary_ctx *ctx, size_t pos) {
    ASSERTAB(pos < ctx->size, "out of memory.");
    return ctx->data + pos;
}
/// <summary>
/// 获取int8
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <returns>int8_t</returns>
static inline int8_t binary_get_int8(binary_ctx *ctx) {
    ASSERTAB(binary_have(ctx, sizeof(int8_t)), "out of memory.");
    int8_t val = (ctx->data + ctx->offset)[0];
    ctx->offset += sizeof(val);
    return val;
}
/// <summary>
/// 获取uint8
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <returns>uint8_t</returns>
static inline uint8_t binary_get_uint8(binary_ctx *ctx) {
    ASSERTAB(binary_have(ctx, sizeof(uint8_t)), "out of memory.");
    uint8_t val = (uint8_t)(ctx->data + ctx->offset)[0];
    ctx->offset += sizeof(val);
    return val;
}
/// <summary>
/// 获取整数值
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="lens">字节数</param>
/// <param name="islittle">1 小端序列 0大端序列</param>
/// <returns>int64_t</returns>
static inline int64_t binary_get_integer(binary_ctx *ctx, size_t lens, int32_t islittle) {
    //先减后比，避免攻击者构造极大 lens 让 offset+lens size_t 溢出绕过断言
    ASSERTAB(binary_have(ctx, lens), "out of memory.");
    int64_t val = unpack_integer(ctx->data + ctx->offset, (int32_t)lens, islittle, 1);
    ctx->offset += lens;
    return val;
}
/// <summary>
/// 获取无符号整数值
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="lens">字节数</param>
/// <param name="islittle">1 小端序列 0大端序列</param>
/// <returns>uint64_t</returns>
static inline uint64_t binary_get_uinteger(binary_ctx *ctx, size_t lens, int32_t islittle) {
    ASSERTAB(binary_have(ctx, lens), "out of memory.");
    uint64_t val = (uint64_t)unpack_integer(ctx->data + ctx->offset, (int32_t)lens, islittle, 0);
    ctx->offset += lens;
    return val;
}
/// <summary>
/// 获取float值
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="islittle">1 小端序列 0大端序列</param>
/// <returns>float</returns>
static inline float binary_get_float(binary_ctx *ctx, int32_t islittle) {
    ASSERTAB(binary_have(ctx, sizeof(float)), "out of memory.");
    float val = unpack_float(ctx->data + ctx->offset, islittle);
    ctx->offset += sizeof(val);
    return val;
}
/// <summary>
/// 获取double值
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="islittle">1 小端序列 0大端序列</param>
/// <returns>double</returns>
static inline double binary_get_double(binary_ctx *ctx, int32_t islittle) {
    ASSERTAB(binary_have(ctx, sizeof(double)), "out of memory.");
    double val = unpack_double(ctx->data + ctx->offset, islittle);
    ctx->offset += sizeof(val);
    return val;
}
/// <summary>
/// 同 binary_get_string,但剩余字节里没有 '\0' 时失败而不是断言。
/// 越界条件是"扫不到 NUL",binary_have 预判不了,所以单给一个接口
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <returns>字符串首址;剩余字节里没有 '\0' 返回 NULL(offset 不动)</returns>
static inline char *binary_try_get_string(binary_ctx *ctx) {
    char *val = ctx->data + ctx->offset;
    size_t remain = binary_remain(ctx);
    size_t slen = strnlen(val, remain);
    if (slen >= remain) {
        return NULL;
    }
    ctx->offset += slen + 1;
    return val;
}
/// <summary>
/// 获取字符串值,取到'\0'结束。剩余字节里没有 '\0' 即 ASSERTAB abort,
/// 长度由对端决定的报文改用 binary_try_get_string
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <returns>char *</returns>
static inline char *binary_get_string(binary_ctx *ctx) {
    char *val = binary_try_get_string(ctx);
    ASSERTAB(NULL != val, "out of memory.");
    return val;
}
/// <summary>
/// 获取指定长度的数据
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="lens">长度</param>
/// <returns>char *;lens 为 0 时返回 NULL</returns>
static inline char *binary_get_binary(binary_ctx *ctx, size_t lens) {
    ASSERTAB(binary_have(ctx, lens), "out of memory.");
    if (0 == lens) {
        return NULL;
    }
    char *val = ctx->data + ctx->offset;
    ctx->offset += lens;
    return val;
}
/// <summary>
/// 跳过指定字节
/// </summary>
/// <param name="ctx">binary_ctx</param>
/// <param name="lens">长度</param>
static inline void binary_get_skip(binary_ctx *ctx, size_t lens) {
    ASSERTAB(binary_have(ctx, lens), "out of memory.");
    ctx->offset += lens;
}

#endif//BINARY_H_
