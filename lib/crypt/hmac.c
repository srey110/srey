#include "crypt/hmac.h"
#include "utils/utils.h"

/* RFC 2104：ipad/opad 长度等于压缩函数输入块大小 B，不是摘要输出长度。
 * SHA-1/SHA-256 的 B = 64，SHA-512 的 B = 128。
 * HMAC_MAX_KEY_LENS 取所有支持算法中最大的 B，用于栈缓冲区声明。 */
#define HMAC_MAX_KEY_LENS 128

static size_t _hmac_key_block(digest_type dtype) {
    if (DG_SHA512 == dtype) {
        return 128;
    }
    if (DG_MD2 == dtype) {
        return 16;
    }
    return 64;
}
void hmac_init(hmac_ctx *hmac, digest_type dtype, const char *key, size_t klens) {
    digest_init(&hmac->inside, dtype);
    digest_init(&hmac->outside, dtype);
    digest_init(&hmac->inside_init, dtype);
    digest_init(&hmac->outside_init, dtype);
    size_t key_block = _hmac_key_block(dtype);
    char *key_used;
    char key_temp[DG_BLOCK_SIZE], block_ipad[HMAC_MAX_KEY_LENS], block_opad[HMAC_MAX_KEY_LENS];
    if (key_block == klens) {
        key_used = (char *)key;
    } else {
        if (klens > key_block) {
            digest_update(&hmac->outside, key, klens);
            klens = digest_final(&hmac->outside, key_temp);
            key_used = key_temp;
        } else {
            key_used = (char *)key;
        }
        int32_t fill = (int32_t)key_block - (int32_t)klens;
        if (fill > 0) {
            memset(block_ipad + klens, 0x36, (size_t)fill);
            memset(block_opad + klens, 0x5c, (size_t)fill);
        }
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
size_t hmac_size(hmac_ctx *hmac) {
    return digest_size(&hmac->outside);
}
void hmac_update(hmac_ctx *hmac, const void *data, size_t lens) {
    digest_update(&hmac->inside, data, lens);
}
size_t hmac_final(hmac_ctx *hmac, char *hash) {
    size_t lens = digest_final(&hmac->inside, hash);
    digest_update(&hmac->outside, hash, lens);
    return digest_final(&hmac->outside, hash);
}
void hmac_reset(hmac_ctx *hmac) {
    memcpy(&hmac->inside.eng_ctx, &hmac->inside_init.eng_ctx, sizeof(hmac->inside_init.eng_ctx));
    memcpy(&hmac->outside.eng_ctx, &hmac->outside_init.eng_ctx, sizeof(hmac->outside_init.eng_ctx));
}
void hmac_free(hmac_ctx *hmac) {
    secure_zero(hmac, sizeof(hmac_ctx));
}
