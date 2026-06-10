#ifndef SERI_H_
#define SERI_H_

#include "utils/binary.h"

typedef enum seri_item_type {
    SERI_ITEM_NIL = 0x00,
    SERI_ITEM_BOOL,
    SERI_ITEM_INT,
    SERI_ITEM_REAL,
    SERI_ITEM_STRING,
    SERI_ITEM_USERDATA,
    SERI_ITEM_ARRAY_BEGIN,
    SERI_ITEM_ARRAY_END,
}seri_item_type;

typedef struct seri_item {
    seri_item_type type;
    union {
        int32_t  b;        // BOOL
        uint32_t array_n;  // ARRAY_BEGIN：数组段长度
        int64_t  i;        // INT
        double   r;        // REAL
        struct { const char *p; size_t len; } s;  // STRING：p 指向 iter->buffer，零拷贝
        void    *ud;       // USERDATA
    } v;
}seri_item;
typedef struct seri_iter {
    size_t len;
    size_t offset;
    const char *buffer;
}seri_iter;

/// <summary>
/// 追加 nil 值（1 字节 tag）
/// </summary>
/// <param name="bw">binary_ctx，需 binary_init(bw, NULL, 0, 0) 内部托管模式</param>
void seri_append_nil(binary_ctx *bw);
/// <summary>
/// 追加 bool 值（1 字节 tag，bool 状态编码在高 5 位 cookie 中）
/// </summary>
/// <param name="bw">binary_ctx</param>
/// <param name="b">非 0 为 true</param>
void seri_append_bool(binary_ctx *bw, int32_t b);
/// <summary>
/// 追加整数；根据 v 范围自动选档：
/// v==0 → ZERO(1 byte)；v 越出 int32 → QWORD(9 byte i64)；v 小于 0 → DWORD(5 byte i32)；
/// v 小于 0x100 → BYTE(2 byte)；v 小于 0x10000 → WORD(3 byte)；其余正数 → DWORD(5 byte u32)。
/// 多字节均小端。
/// </summary>
void seri_append_int(binary_ctx *bw, int64_t v);
/// <summary>
/// 追加实数（1 byte tag + 8 byte double 小端）
/// </summary>
void seri_append_real(binary_ctx *bw, double v);
/// <summary>
/// 追加字符串（二进制安全）；len 小于 32 → 短字符串内联 cookie 长度；
/// len 小于 65536 → 长字符串 + u16 长度前缀；其余 → + u32 长度前缀。
/// </summary>
/// <param name="bw">binary_ctx</param>
/// <param name="s">payload；len==0 时可为 NULL</param>
/// <param name="len">字节数</param>
void seri_append_string(binary_ctx *bw, const char *s, size_t len);
/// <summary>
/// 追加裸指针（lightuserdata），sizeof(void*) 字节小端。跨进程禁用（指针在另一进程无效）。
/// </summary>
void seri_append_userdata(binary_ctx *bw, void *ud);
/// <summary>
/// 开始 array 段；array_n 为数组段元素数。
/// array_n 小于 31 时 cookie 直接编码 array_n；array_n 大于等于 31 时 cookie=31 后跟一个完整 INT 写真实长度。
/// 调用契约：随后必须连续追加恰好 array_n 个值（任意类型），
/// 然后写入 hash 段（key/value 对，key 不可为 nil），最后调 seri_append_array_end 结束。
/// </summary>
void seri_append_array_start(binary_ctx *bw, uint32_t array_n);
/// <summary>
/// 结束 array（写 NIL 标记 hash 段结束）；与 seri_append_array_start 配对使用。
/// </summary>
void seri_append_array_end(binary_ctx *bw);
/// <summary>
/// 初始化 iter 视图（零拷贝）；buf 生命周期需 ≥ iter 使用期间（item 中字符串指针依赖此）。
/// </summary>
/// <param name="it">seri_iter</param>
/// <param name="buf">序列化数据起点</param>
/// <param name="size">字节数</param>
void seri_iter_init(seri_iter *it, const void *buf, size_t size);
/// <summary>
/// 解码下一个值；调用方循环调用直到返回 0（流结束）或 -1（格式错）。
/// 遇到 SERI_ITEM_ARRAY_BEGIN 时调用方需消费后续 v.array_n 个数组元素，
/// 然后循环读 key/value 对直到 SERI_ITEM_ARRAY_END。
/// </summary>
/// <param name="it">seri_iter</param>
/// <param name="out">出参：解码后的 item（字符串 p 指向 it->buffer 内部，零拷贝）</param>
/// <returns>1=有 item / 0=流结束 / -1=格式错</returns>
int32_t seri_iter_next(seri_iter *it, seri_item *out);

#endif//SERI_H_
