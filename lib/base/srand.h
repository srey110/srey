#ifndef SRAND_H_
#define SRAND_H_

#include "base/os.h"

/// <summary>
/// 随机[min, max]
/// </summary>
/// <param name="min">最小</param>
/// <param name="max">最大</param>
/// <returns>值</returns>
int32_t randrange(int32_t min, int32_t max);
/// <summary>
/// 随机字符串。伪随机，不能用于密钥、nonce 这类要求不可预测的场合，那类用 csprng_rand
/// </summary>
/// <param name="buf">buffer，必须至少分配 len+1 字节（末尾写 '\0'）</param>
/// <param name="len">随机字符数</param>
/// <returns>buf 本身，已写 len 个 [A-Za-z0-9] 字符并在 buf[len] 补 '\0'</returns>
char *randstr(char *buf, size_t len);
/// <summary>
/// 用密码学安全随机数（CSPRNG）填充缓冲区。
/// 各平台实现：Windows=BCryptGenRandom，Darwin/BSD=arc4random_buf，
/// Linux=getrandom syscall（头文件或内核不支持、或被 seccomp 拦下报 EPERM 时退到 /dev/urandom），其余 Unix=/dev/urandom。
/// </summary>
/// <param name="buf">目标缓冲区</param>
/// <param name="len">填充字节数</param>
/// <returns>ERR_OK 成功，ERR_FAILED 失败</returns>
int32_t csprng_rand(void *buf, size_t len);

#endif//SRAND_H_
