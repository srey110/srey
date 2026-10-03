#ifndef SBYTESWAP_H_
#define SBYTESWAP_H_

#include "base/os.h"

/// <summary>
/// 数字转 char*
/// </summary>
/// <param name="buf">buffer</param>
/// <param name="val">数字</param>
/// <param name="size">字节数</param>
/// <param name="islittle">是否为小端</param>
void pack_integer(char *buf, uint64_t val, int32_t size, int32_t islittle);
/// <summary>
/// char* 转数字
/// </summary>
/// <param name="buf">要转换的buffer</param>
/// <param name="size">字节数</param>
/// <param name="islittle">是否为小端</param>
/// <param name="issigned">是否有符号</param>
/// <returns>数字</returns>
int64_t unpack_integer(const char *buf, int32_t size, int32_t islittle, int32_t issigned);
/// <summary>
/// float转 char*
/// </summary>
/// <param name="buf">buffer</param>
/// <param name="val">值</param>
/// <param name="islittle">是否为小端</param>
void pack_float(char *buf, float val, int32_t islittle);
/// <summary>
/// char* 转float
/// </summary>
/// <param name="buf">要转换的buffer</param>
/// <param name="islittle">是否为小端</param>
/// <returns>float</returns>
float unpack_float(const char *buf, int32_t islittle);
/// <summary>
/// double转 char*
/// </summary>
/// <param name="buf">buffer</param>
/// <param name="val">值</param>
/// <param name="islittle">是否为小端</param>
void pack_double(char *buf, double val, int32_t islittle);
/// <summary>
/// char* 转double
/// </summary>
/// <param name="buf">要转换的buffer</param>
/// <param name="islittle">是否为小端</param>
/// <returns>double</returns>
double unpack_double(const char *buf, int32_t islittle);
#if !defined(OS_WIN) && !defined(OS_DARWIN) && !defined(OS_AIX)
/// <summary>
/// 64 位网络字节序转主机字节序
/// </summary>
/// <param name="val">网络字节序值</param>
/// <returns>主机字节序值</returns>
uint64_t ntohll(uint64_t val);
/// <summary>
/// 64 位主机字节序转网络字节序
/// </summary>
/// <param name="val">主机字节序值</param>
/// <returns>网络字节序值</returns>
uint64_t htonll(uint64_t val);
#endif//OS_WIN OS_DARWIN OS_AIX
// 字节序，三组：byteswap16/32/64 把整数的字节倒过来排；read_be* / read_le* 从 p 按大端 / 小端取整数；
// write_be* / write_le* 把 v 按大端 / 小端写到 p。结果与本机字节序无关，p 不要求对齐，
// 一次读写 位数/8 个字节。契约写在每组 16 位那个上，其余"同 xxx"
/// <summary>
/// 16 位整数换字节序
/// </summary>
/// <param name="v">原值</param>
/// <returns>字节倒过来排后的值</returns>
static inline uint16_t byteswap16(uint16_t v) {
#if defined(OS_WIN)
    return _byteswap_ushort(v);// MSVC 内建：2 字节对调
#else
    return (uint16_t)((v << 8) | (v >> 8));
#endif
}
/// <summary>
/// 32 位换序，同 byteswap16
/// </summary>
static inline uint32_t byteswap32(uint32_t v) {
#if defined(OS_WIN)
    return _byteswap_ulong(v);// MSVC 内建：4 字节倒过来排
#else
    return (v << 24) | ((v << 8) & 0x00FF0000U) | ((v >> 8) & 0x0000FF00U) | (v >> 24);
#endif
}
/// <summary>
/// 64 位换序，同 byteswap16
/// </summary>
static inline uint64_t byteswap64(uint64_t v) {
#if defined(OS_WIN)
    return _byteswap_uint64(v);// MSVC 内建：8 字节倒过来排
#else
    return ((uint64_t)byteswap32((uint32_t)v) << 32) | byteswap32((uint32_t)(v >> 32));
#endif
}
/// <summary>
/// 按大端读 2 字节
/// </summary>
/// <param name="p">起址，不要求对齐，须有 2 字节可读</param>
/// <returns>读出的整数</returns>
static inline uint16_t read_be16(const void *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return IS_LITTLE ? byteswap16(v) : v;
}
/// <summary>
/// 按大端读 4 字节，同 read_be16
/// </summary>
static inline uint32_t read_be32(const void *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return IS_LITTLE ? byteswap32(v) : v;
}
/// <summary>
/// 按大端读 8 字节，同 read_be16
/// </summary>
static inline uint64_t read_be64(const void *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return IS_LITTLE ? byteswap64(v) : v;
}
/// <summary>
/// 按小端读 2 字节，同 read_be16
/// </summary>
static inline uint16_t read_le16(const void *p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return IS_LITTLE ? v : byteswap16(v);
}
/// <summary>
/// 按小端读 4 字节，同 read_be16
/// </summary>
static inline uint32_t read_le32(const void *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return IS_LITTLE ? v : byteswap32(v);
}
/// <summary>
/// 按小端读 8 字节，同 read_be16
/// </summary>
static inline uint64_t read_le64(const void *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return IS_LITTLE ? v : byteswap64(v);
}
/// <summary>
/// 按大端写 2 字节
/// </summary>
/// <param name="p">目标起址，不要求对齐，须有 2 字节可写</param>
/// <param name="v">要写的整数</param>
static inline void write_be16(void *p, uint16_t v) {
    if (IS_LITTLE) {
        v = byteswap16(v);
    }
    memcpy(p, &v, sizeof(v));
}
/// <summary>
/// 按大端写 4 字节，同 write_be16
/// </summary>
static inline void write_be32(void *p, uint32_t v) {
    if (IS_LITTLE) {
        v = byteswap32(v);
    }
    memcpy(p, &v, sizeof(v));
}
/// <summary>
/// 按大端写 8 字节，同 write_be16
/// </summary>
static inline void write_be64(void *p, uint64_t v) {
    if (IS_LITTLE) {
        v = byteswap64(v);
    }
    memcpy(p, &v, sizeof(v));
}
/// <summary>
/// 按小端写 2 字节，同 write_be16
/// </summary>
static inline void write_le16(void *p, uint16_t v) {
    if (!IS_LITTLE) {
        v = byteswap16(v);
    }
    memcpy(p, &v, sizeof(v));
}
/// <summary>
/// 按小端写 4 字节，同 write_be16
/// </summary>
static inline void write_le32(void *p, uint32_t v) {
    if (!IS_LITTLE) {
        v = byteswap32(v);
    }
    memcpy(p, &v, sizeof(v));
}
/// <summary>
/// 按小端写 8 字节，同 write_be16
/// </summary>
static inline void write_le64(void *p, uint64_t v) {
    if (!IS_LITTLE) {
        v = byteswap64(v);
    }
    memcpy(p, &v, sizeof(v));
}
/// <summary>
/// 按字节数读整数：2 / 4 / 8 字节走上面的定长读，其余长度交给 unpack_integer
/// </summary>
/// <param name="p">数据起点，不要求对齐</param>
/// <param name="size">字节数</param>
/// <param name="islittle">1 小端 0 大端</param>
/// <param name="issigned">1 按有符号做符号扩展，0 零扩展</param>
/// <returns>读出的值</returns>
static inline int64_t read_integer(const void *p, size_t size, int32_t islittle, int32_t issigned) {
    uint16_t v16;
    uint32_t v32;
    switch (size) {
    case 2:
        v16 = islittle ? read_le16(p) : read_be16(p);
        return issigned ? (int64_t)(int16_t)v16 : (int64_t)v16;
    case 4:
        v32 = islittle ? read_le32(p) : read_be32(p);
        return issigned ? (int64_t)(int32_t)v32 : (int64_t)v32;
    case 8:
        return (int64_t)(islittle ? read_le64(p) : read_be64(p));
    default:
        return unpack_integer((const char *)p, (int32_t)size, islittle, issigned);
    }
}
/// <summary>
/// 按字节数写 val 的低 size 字节：2 / 4 / 8 字节走上面的定长写，其余长度交给 pack_integer
/// </summary>
/// <param name="p">写入起点，不要求对齐</param>
/// <param name="val">值</param>
/// <param name="size">字节数</param>
/// <param name="islittle">1 小端 0 大端</param>
static inline void write_integer(void *p, uint64_t val, size_t size, int32_t islittle) {
    switch (size) {
    case 2:
        islittle ? write_le16(p, (uint16_t)val) : write_be16(p, (uint16_t)val);
        break;
    case 4:
        islittle ? write_le32(p, (uint32_t)val) : write_be32(p, (uint32_t)val);
        break;
    case 8:
        islittle ? write_le64(p, val) : write_be64(p, val);
        break;
    default:
        pack_integer((char *)p, val, (int32_t)size, islittle);
        break;
    }
}

#endif//SBYTESWAP_H_
