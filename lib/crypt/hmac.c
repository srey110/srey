#include "crypt/hmac.h"
#include "utils/utils.h"

/* RFC 2104：ipad/opad 长度等于压缩函数输入块大小 B，不是摘要输出长度。B 由 digest_init
 * 一并写进 digest_ctx.key_block —— 算法属性只留一张表，新增算法漏填会被那里的 default 断言挡住。
 * HMAC_MAX_KEY_LENS 取所有支持算法中最大的 B，用于栈缓冲区声明。 */
#define HMAC_MAX_KEY_LENS 128

void hmac_init(hmac_ctx *hmac, digest_type dtype, const char *key, size_t klens) {
    digest_init(&hmac->inside, dtype);
    digest_init(&hmac->outside, dtype);
    digest_init(&hmac->inside_init, dtype);
    digest_init(&hmac->outside_init, dtype);
    size_t key_block = hmac->inside.key_block;
    ASSERTAB(key_block <= HMAC_MAX_KEY_LENS, "key block exceeds stack buffer.");
    char *key_used;
    char key_temp[DG_BLOCK_SIZE], block_ipad[HMAC_MAX_KEY_LENS], block_opad[HMAC_MAX_KEY_LENS];
    key_used = (char *)key;
    if (klens > key_block) {
        // 超块长的 key 先摘要成 digest 长度再用
        digest_update(&hmac->outside, key, klens);
        klens = digest_final(&hmac->outside, key_temp);
        key_used = key_temp;
    }
    if (klens < key_block) {
        // 不足块长的部分直接就是 pad 值(等价于先补 0 再异或)
        memset(block_ipad + klens, 0x36, key_block - klens);
        memset(block_opad + klens, 0x5c, key_block - klens);
    }
    for (size_t i = 0; i < klens; i++) {
        block_ipad[i] = key_used[i] ^ 0x36;
        block_opad[i] = key_used[i] ^ 0x5c;
    }
    digest_update(&hmac->inside_init, block_ipad, key_block);
    digest_update(&hmac->outside_init, block_opad, key_block);
    hmac_reset(hmac);
    secure_zero(key_temp, sizeof(key_temp));
    secure_zero(block_ipad, sizeof(block_ipad));
    secure_zero(block_opad, sizeof(block_opad));
}
void hmac_free(hmac_ctx *hmac) {
    secure_zero(hmac, sizeof(hmac_ctx));
}
size_t hmac_size(hmac_ctx *hmac) {
    return digest_size(&hmac->outside);
}
void hmac_update(hmac_ctx *hmac, const void *data, size_t lens) {
    digest_update(&hmac->inside, data, lens);
}
// digest_final 只把两个 digest 复位到算法 IV，而 HMAC 要的是 ipad/opad 吸收之后的状态,
// 少这一步则第二次 final 变成"无密钥"的摘要，对任何密钥都得到同一个常量
size_t hmac_final(hmac_ctx *hmac, char *hash) {
    size_t lens = digest_final(&hmac->inside, hash);
    digest_update(&hmac->outside, hash, lens);
    lens = digest_final(&hmac->outside, hash);
    hmac_reset(hmac);
    return lens;
}
// 按当前引擎的实际 ctx 大小拷，不按联合体整份：PBKDF2 每轮要拷两次，多拷的部分随迭代次数放大
void hmac_reset(hmac_ctx *hmac) {
    ASSERTAB(hmac->inside_init.eng_lens <= sizeof(hmac->inside_init.eng_ctx)
             && hmac->outside_init.eng_lens <= sizeof(hmac->outside_init.eng_ctx),
             "hmac not initialized.");
    memcpy(&hmac->inside.eng_ctx, &hmac->inside_init.eng_ctx, hmac->inside_init.eng_lens);
    memcpy(&hmac->outside.eng_ctx, &hmac->outside_init.eng_ctx, hmac->outside_init.eng_lens);
}
