#ifndef CRYPT_PUB_H_
#define CRYPT_PUB_H_

#include "base/base.h"

// 空汇编：告诉编译器 v 在寄存器里被读写过，它看不透 v 的值，就不会把前后用到 v 的计算合并、重排或自动向量化。
// md4 / md5 靠它保住每步"先加与 b 无关的项"的顺序；别删
#if defined(CC_GNU)
#define CRYPT_OPAQUE(v) __asm__("" : "+r"(v))
#else
#define CRYPT_OPAQUE(v) (void)0
#endif// CC_GNU
// SHA-2 连续 8 轮：工作变量 a~h 不搬动，每轮把名字轮换一位；RND(i,a,b,c,d,e,f,g,h) 为单轮宏。sha256 / sha512 共用
#define CRYPT_ROUNDS8(RND, i) do { \
    RND((i) + 0, a, b, c, d, e, f, g, h); \
    RND((i) + 1, h, a, b, c, d, e, f, g); \
    RND((i) + 2, g, h, a, b, c, d, e, f); \
    RND((i) + 3, f, g, h, a, b, c, d, e); \
    RND((i) + 4, e, f, g, h, a, b, c, d); \
    RND((i) + 5, d, e, f, g, h, a, b, c); \
    RND((i) + 6, c, d, e, f, g, h, a, b); \
    RND((i) + 7, b, c, d, e, f, g, h, a); \
} while (0)

/// <summary>
/// 恒定时间内存比较，防止时序攻击。
/// 无论差异位置在哪，均遍历全部字节后返回，执行时间与内容无关。
/// </summary>
/// <param name="a">缓冲区 a</param>
/// <param name="b">缓冲区 b</param>
/// <param name="len">比较长度（字节）</param>
/// <returns>相等返回 0，不相等返回非 0</returns>
static inline int32_t crypt_memcmp(const void *a, const void *b, size_t len) {
    // 输入按 volatile 读：每个字节都得真读一遍，编译器没法提前退出；只异或再按位或，不按内容走分支
    const volatile unsigned char *pa = (const volatile unsigned char *)a;
    const volatile unsigned char *pb = (const volatile unsigned char *)b;
    unsigned char diff = 0;
    size_t i;
    for (i = 0; i < len; i++) {
        diff |= pa[i] ^ pb[i];
    }
    return (int32_t)diff;
}

#endif//CRYPT_PUB_H_
