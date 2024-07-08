#ifndef HMAC_H_
#define HMAC_H_

#include "crypt/digest.h"

typedef struct hmac_ctx {
    digest_ctx inside;
    digest_ctx outside;
    digest_ctx inside_init;
    digest_ctx outside_init;
}hmac_ctx;

void hmac_init(hmac_ctx *hmac, digest_type dtype, const char *key, size_t klens);
size_t hmac_size(hmac_ctx *hmac);
void hmac_update(hmac_ctx *hmac, const void *data, size_t lens);
//DG_BLOCK_SIZE
size_t hmac_final(hmac_ctx *hmac, char *hash);
void hmac_reset(hmac_ctx *hmac);

#endif//HMAC_H_
