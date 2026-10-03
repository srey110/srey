#ifndef HMAC_H_
#define HMAC_H_

#include "crypt/digest.h"

typedef struct hmac_ctx {
    digest_ctx inside;        // 内层摘要上下文（当前计算中）
    digest_ctx outside;       // 外层摘要上下文（当前计算中）
    digest_ctx inside_init;   // 内层摘要初始状态（用于 reset）
    digest_ctx outside_init;  // 外层摘要初始状态（用于 reset）
}hmac_ctx;

/// <summary>
/// HMAC 初始化
/// </summary>
/// <param name="hmac">hmac_ctx</param>
/// <param name="dtype">摘要算法，只接受 DG_MD2..DG_SHA512，传 xxhash 会被 ASSERTAB 终止</param>
/// <param name="key">密码</param>
/// <param name="klens">密码长度</param>
void hmac_init(hmac_ctx *hmac, digest_type dtype, const char *key, size_t klens);
/// <summary>
/// 清零整个 hmac_ctx，含内嵌 digest_ctx 的分发回调——调用后上下文即失效，要复用须重新 hmac_init
/// </summary>
/// <param name="hmac">hmac_ctx</param>
void hmac_free(hmac_ctx *hmac);
/// <summary>
/// 获取hash长度
/// </summary>
/// <param name="hmac">hmac_ctx</param>
/// <returns>长度</returns>
size_t hmac_size(hmac_ctx *hmac);
/// <summary>
/// 填入数据
/// </summary>
/// <param name="hmac">hmac_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">数据长度</param>
void hmac_update(hmac_ctx *hmac, const void *data, size_t lens);
/// <summary>
/// 计算hash
/// </summary>
/// <param name="hmac">hmac_ctx</param>
/// <param name="hash">输出缓冲，容量须 >= hmac_init 所选算法的输出长度（即 hmac_size 的返回值）；
/// 算法在编译期不确定时按 DG_BLOCK_SIZE 给，那是所有支持算法里最长的一个</param>
/// <returns>hash长度。返回后上下文已自动复位（密钥仍在），可直接开始下一条消息（无需再调 hmac_reset）</returns>
size_t hmac_final(hmac_ctx *hmac, char *hash);
/// <summary>
/// 重置,准备新一轮计算
/// </summary>
/// <param name="hmac">hmac_ctx</param>
void hmac_reset(hmac_ctx *hmac);
/// <summary>
/// PBKDF2（RFC 8018）的第一个输出块：U1 = HMAC(P, salt || INT(1))，Ui = HMAC(P, Ui-1)，结果是 U1 ^ ... ^ Uiter。
/// 输出长度即摘要长度，需要更长输出的派生不适用
/// </summary>
/// <param name="hmac">已用口令 hmac_init 过、且未 update 过的上下文；返回后已复位（密钥仍在）</param>
/// <param name="salt">盐</param>
/// <param name="slens">盐长度</param>
/// <param name="iter">迭代次数，小于 1 按 1 算</param>
/// <param name="out">输出缓冲，容量须 >= hmac_size 的返回值</param>
/// <returns>输出长度</returns>
size_t hmac_pbkdf2(hmac_ctx *hmac, const void *salt, size_t slens, int32_t iter, char *out);

#endif//HMAC_H_
