#ifndef HMAC_H_
#define HMAC_H_

#include "algo/md5.h"
#include "algo/sha1.h"
#include "algo/sha256.h"
//https://github.com/ogay/hmac

#define HMAC_DECL(hs_type, hmac_type, block_size, trans_size, _hs_init, _hs_update, _hs_final)\
typedef struct hmac_type {\
    hs_type inside;\
    hs_type outside;\
    hs_type inside_init;\
    hs_type outside_init;\
}hmac_type##_ctx;\
static inline void hmac_type##_key(hmac_type##_ctx *ctx, unsigned char *key ,size_t klens) {\
    size_t num;\
    unsigned char *key_used;\
    unsigned char key_temp[block_size], block_ipad[trans_size], block_opad[trans_size];\
    if (trans_size == klens) {\
        key_used = key;\
        num = trans_size;\
    } else {\
        if (klens > trans_size) {\
            num = block_size;\
            hs_type hs;\
            _hs_init(&hs);\
            _hs_update(&hs, key, klens);\
            _hs_final(&hs, key_temp);\
            key_used = key_temp;\
        } else {\
            key_used = key;\
            num = klens;\
        }\
        size_t fill = trans_size - num;\
        memset(block_ipad + num, 0x36, fill);\
        memset(block_opad + num, 0x5c, fill);\
    }\
    for (size_t i = 0; i < num; i++) {\
        block_ipad[i] = key_used[i] ^ 0x36;\
        block_opad[i] = key_used[i] ^ 0x5c;\
    }\
    _hs_init(&ctx->inside_init);\
    _hs_update(&ctx->inside_init, block_ipad, sizeof(block_ipad));\
    _hs_init(&ctx->outside_init);\
    _hs_update(&ctx->outside_init, block_opad, sizeof(block_opad));\
};\
static inline void hmac_type##_init(hmac_type##_ctx *ctx) {\
    memcpy(&ctx->inside, &ctx->inside_init, sizeof(hs_type));\
    memcpy(&ctx->outside, &ctx->outside_init, sizeof(hs_type));\
};\
static inline void hmac_type##_update(hmac_type##_ctx *ctx, unsigned char *data, size_t lens) {\
    _hs_update(&ctx->inside, data, lens);\
};\
static inline void hmac_type##_final(hmac_type##_ctx *ctx, unsigned char mac[block_size]) {\
    _hs_final(&ctx->inside, mac);\
    _hs_update(&ctx->outside, mac, block_size);\
    _hs_final(&ctx->outside, mac);\
};\

HMAC_DECL(sha256_ctx, hmac_sha256, SHA256_BLOCK_SIZE, 64, sha256_init, sha256_update, sha256_final);
HMAC_DECL(sha1_ctx, hmac_sha1, SHA1_BLOCK_SIZE, 64, sha1_init, sha1_update, sha1_final);
HMAC_DECL(md5_ctx, hmac_md5, MD5_BLOCK_SIZE, 64, md5_init, md5_update, md5_final);

#endif//HMAC_H_
