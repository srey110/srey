#include "crypt/hmac.h"

#define HMAC_KEY_LENS 64

void hmac_init(hmac_ctx *hmac, digest_type dtype, const char *key, size_t klens) {
    digest_init(&hmac->inside, dtype);
    digest_init(&hmac->outside, dtype);
    digest_init(&hmac->inside_init, dtype);
    digest_init(&hmac->outside_init, dtype);
    char *key_used;
    char key_temp[DG_BLOCK_SIZE], block_ipad[HMAC_KEY_LENS], block_opad[HMAC_KEY_LENS];
    if (HMAC_KEY_LENS == klens) {
        key_used = (char *)key;
    } else {
        if (klens > HMAC_KEY_LENS) {
            digest_update(&hmac->outside, key, klens);
            klens = digest_final(&hmac->outside, key_temp);
            key_used = key_temp;
        } else {
            key_used = (char *)key;
        }
        int32_t fill = HMAC_KEY_LENS - (int32_t)klens;
        if (fill > 0) {
            memset(block_ipad + klens, 0x36, (size_t)fill);
            memset(block_opad + klens, 0x5c, (size_t)fill);
        }
    }
    for (size_t i = 0; i < klens; i++) {
        block_ipad[i] = key_used[i] ^ 0x36;
        block_opad[i] = key_used[i] ^ 0x5c;
    }
    digest_update(&hmac->inside_init, block_ipad, sizeof(block_ipad));
    digest_update(&hmac->outside_init, block_opad, sizeof(block_opad));
    hmac_reset(hmac);
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
