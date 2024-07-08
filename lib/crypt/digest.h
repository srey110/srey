#ifndef DIGEST_H_
#define DIGEST_H_

#include "crypt/md2.h"
#include "crypt/md4.h"
#include "crypt/md5.h"
#include "crypt/sha1.h"
#include "crypt/sha256.h"
#include "crypt/sha512.h"

#define DG_BLOCK_SIZE SHA512_BLOCK_SIZE
typedef void(*_init_cb)(void *);
typedef void(*_update_cb)(void *, const void *, size_t);
typedef void(*_final_cb)(void *, char *);

typedef enum digest_type {
    DG_MD2 = 0x01,
    DG_MD4,
    DG_MD5,
    DG_SHA1,
    DG_SHA256,
    DG_SHA512
}digest_type;
typedef struct digest_ctx {
    size_t block_lens;
    void *cur_ctx;
    _init_cb _init;
    _update_cb _update;
    _final_cb _final;
    union {
        md2_ctx md2;
        md4_ctx md4;
        md5_ctx md5;
        sha1_ctx sha1;
        sha256_ctx sha256;
        sha512_ctx sha512;
    }eng_ctx;
}digest_ctx;

void digest_init(digest_ctx *digest, digest_type dtype);
size_t digest_size(digest_ctx *digest);
void digest_update(digest_ctx *digest, const void *data, size_t lens);
//DG_BLOCK_SIZE
size_t digest_final(digest_ctx *digest, char *hash);
void digest_reset(digest_ctx *digest);

#endif//DIGEST_H_
