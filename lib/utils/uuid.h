#ifndef UUID_H_
#define UUID_H_

#include "base/base.h"

// RFC 9562 的 v4（随机）与 v7（按时间排序）。UUID 是 16 字节二进制，比较直接 memcmp(a, b, UUID_LENS)，
// v7 的字节序就是时间序。Nil / Max 用 ZERO / memset(0xFF) 构造

#define UUID_STR_LENS 37// 36 个字符加结尾 '\0'

/// <summary>
/// 生成 v4 UUID（122 位随机）
/// </summary>
/// <param name="uuid">输出；失败时内容无定义</param>
/// <returns>成功 ERR_OK；随机源失败 ERR_FAILED</returns>
int32_t uuid_v4(char uuid[UUID_LENS]);
/// <summary>
/// 生成 v7 UUID（48 位 Unix 毫秒 + 12 位计数器 + 62 位随机）。任意线程可直接调用，不加锁、不阻塞；
/// 同一进程内按 memcmp 严格递增。同一毫秒生成过多或时钟回拨时，时间戳会暂时领先真实时间
/// </summary>
/// <param name="uuid">输出；失败时内容无定义</param>
/// <returns>成功 ERR_OK；随机源失败 ERR_FAILED，此时内部状态不变</returns>
int32_t uuid_v7(char uuid[UUID_LENS]);
/// <summary>
/// 取版本号，只认 RFC 9562 的变体（第 8 字节高两位为 10）
/// </summary>
/// <param name="uuid">UUID</param>
/// <returns>版本号 0~15；变体不符（含 Nil、Max）返回 0</returns>
int32_t uuid_version(const char uuid[UUID_LENS]);
/// <summary>
/// 取 v7 UUID 里的 Unix 毫秒时间戳
/// </summary>
/// <param name="uuid">UUID</param>
/// <returns>毫秒数；不是 v7 返回 0</returns>
uint64_t uuid_v7_ms(const char uuid[UUID_LENS]);
/// <summary>
/// 格式化成小写 8-4-4-4-12 文本
/// </summary>
/// <param name="uuid">UUID</param>
/// <param name="out">输出，至少 UUID_STR_LENS 字节，末尾写 '\0'</param>
void uuid_tostr(const char uuid[UUID_LENS], char out[UUID_STR_LENS]);
/// <summary>
/// 解析 8-4-4-4-12 文本，十六进制大小写都接受；不接受 {} 与 urn:uuid: 前缀
/// </summary>
/// <param name="str">文本，不要求以 '\0' 结尾</param>
/// <param name="lens">str 字节数，必须恰好是 36</param>
/// <param name="uuid">输出；失败时内容无定义</param>
/// <returns>成功 ERR_OK；长度、连字符位置或字符非法 ERR_FAILED</returns>
int32_t uuid_fromstr(const char *str, size_t lens, char uuid[UUID_LENS]);

#endif//UUID_H_
