#ifndef BITS_H_
#define BITS_H_

#include "base/os.h"

// 位运算与算术小工具

/// <summary>
/// 32 位整数最低那个 1 在第几位(从 0 数)，即末尾连续 0 的个数
/// </summary>
/// <param name="v">待查值，必须非 0(为 0 时结果未定义)</param>
/// <returns>最低置位的下标，0~31</returns>
static inline uint32_t ctz32(uint32_t v) {
#if defined(CC_GNU)
    return (uint32_t)__builtin_ctz(v);// gcc/clang 内建：数末尾连续 0 的个数，即最低那个 1 的位置
#elif defined(OS_WIN)
    unsigned long i;
    _BitScanForward(&i, v);// MSVC 内建：从第 0 位往高位找第一个 1，位置写进 i
    return (uint32_t)i;
#else
    uint32_t n = 0;
    while (0 == (v & 1)) {
        v >>= 1;
        n++;
    }
    return n;
#endif
}
/// <summary>
/// 同 ctz32，64 位版
/// </summary>
/// <param name="v">待查值，必须非 0(为 0 时结果未定义)</param>
/// <returns>最低置位的下标，0~63</returns>
static inline uint32_t ctz64(uint64_t v) {
#if defined(CC_GNU)
    return (uint32_t)__builtin_ctzll(v);// gcc/clang 内建：64 位版，数末尾连续 0 的个数
#elif defined(OS_WIN) && (defined(_M_X64) || defined(_M_ARM64) || defined(_M_ARM64EC))
    unsigned long i;
    _BitScanForward64(&i, v);// 同 _BitScanForward，64 位版
    return (uint32_t)i;
#else
    // 没有 64 位内建(32 位 Windows 等)：低 32 位里有 1 就查低半，否则查高半再加 32
    return 0 != (uint32_t)v ? ctz32((uint32_t)v) : 32 + ctz32((uint32_t)(v >> 32));
#endif
}
/// <summary>
/// 64 位整数最高那个 1 之上有几个 0，即前导 0 的个数
/// </summary>
/// <param name="v">待查值，必须非 0(为 0 时结果未定义)</param>
/// <returns>前导 0 的个数，0~63</returns>
static inline uint32_t clz64(uint64_t v) {
#if defined(CC_GNU)
    return (uint32_t)__builtin_clzll(v);// gcc/clang 内建：数最高位往下连续 0 的个数
#elif defined(OS_WIN) && (defined(_M_X64) || defined(_M_ARM64) || defined(_M_ARM64EC))
    unsigned long i;
    _BitScanReverse64(&i, v);// MSVC 内建：从高位往低位找第一个 1，位置写进 i；63 减它就是前导 0 个数
    return 63 - (uint32_t)i;
#else
    uint32_t n = 0;
    while (0 == (v & ((uint64_t)1 << 63))) {
        v <<= 1;
        n++;
    }
    return n;
#endif
}
/// <summary>
/// 64x64 位无符号乘法，得到完整 128 位积
/// </summary>
/// <param name="a">乘数</param>
/// <param name="b">乘数</param>
/// <param name="hi">输出：积的高 64 位，必须非 NULL</param>
/// <returns>积的低 64 位</returns>
static inline uint64_t mul64_128(uint64_t a, uint64_t b, uint64_t *hi) {
#if defined(__SIZEOF_INT128__)
    unsigned __int128 p = (unsigned __int128)a * b;// gcc/clang 的 128 位整数，64 位平台上编成一两条乘法
    *hi = (uint64_t)(p >> 64);
    return (uint64_t)p;
#elif defined(OS_WIN) && defined(_M_X64) && !defined(_M_ARM64EC)
    return _umul128(a, b, hi);// x64 一条乘法同时给出高、低 64 位
#elif defined(OS_WIN) && (defined(_M_ARM64) || defined(_M_ARM64EC))
    *hi = __umulh(a, b);// ARM64 只取乘积的高 64 位，低 64 位就是普通的 a * b
    return a * b;
#else
    // 没有 128 位乘法的平台：拆成四个 32x32 乘积再拼
    uint64_t lolo = (a & 0xFFFFFFFFULL) * (b & 0xFFFFFFFFULL);
    uint64_t hilo = (a >> 32) * (b & 0xFFFFFFFFULL);
    uint64_t lohi = (a & 0xFFFFFFFFULL) * (b >> 32);
    uint64_t hihi = (a >> 32) * (b >> 32);
    uint64_t cross = (lolo >> 32) + (hilo & 0xFFFFFFFFULL) + lohi;
    *hi = (hilo >> 32) + (cross >> 32) + hihi;
    return (cross << 32) | (lolo & 0xFFFFFFFFULL);
#endif
}
/// <summary>
/// 将 n 向上取整到最近的 2 的幂（uint32_t 范围）。
/// </summary>
/// <param name="n">输入值</param>
/// <returns>最接近且不小于 n 的 2 的幂；n 已是 2 的幂时原值返回，0 返回 0；
/// 大于 0x80000000u 时 ASSERTAB 中止（uint32 无法表示更大的 2 的幂）</returns>
uint32_t pow2_ceil(uint32_t n);

#endif//BITS_H_
