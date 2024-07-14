#ifndef HMAC_H_
#define HMAC_H_

#include "crypt/digest.h"

typedef struct hmac_ctx {
    digest_ctx inside;
    digest_ctx outside;
    digest_ctx inside_init;
    digest_ctx outside_init;
}hmac_ctx;
/// <summary>
/// HMAC 初始化
/// </summary>
/// <param name="hmac">hmac_ctx</param>
/// <param name="dtype">摘要算法</param>
/// <param name="key">密码</param>
/// <param name="klens">密码长度</param>
void hmac_init(hmac_ctx *hmac, digest_type dtype, const char *key, size_t klens);
/// <summary>
/// 获取hash长度
/// </summary>
/// <param name="hmac">hmac_ctx</param>
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
/// <param name="hash">hash, hash[DG_BLOCK_SIZE]</param>
/// <returns>hash长度</returns>
size_t hmac_final(hmac_ctx *hmac, char *hash);
/// <summary>
/// 重置,准备新一轮计算
/// </summary>
/// <param name="hmac">hmac_ctx</param>
void hmac_reset(hmac_ctx *hmac);

#endif//HMAC_H_
