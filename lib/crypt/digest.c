#include "crypt/digest.h"
#include "utils/utils.h"

void digest_init(digest_ctx *digest, digest_type dtype) {
    switch (dtype) {
    case DG_MD2:
        digest->block_lens = MD2_BLOCK_SIZE;
        digest->_init = (_init_cb)md2_init;
        digest->_update = (_update_cb)md2_update;
        digest->_final = (_final_cb)md2_final;
        break;
    case DG_MD4:
        digest->block_lens = MD4_BLOCK_SIZE;
        digest->_init = (_init_cb)md4_init;
        digest->_update = (_update_cb)md4_update;
        digest->_final = (_final_cb)md4_final;
        break;
    case DG_MD5:
        digest->block_lens = MD5_BLOCK_SIZE;
        digest->_init = (_init_cb)md5_init;
        digest->_update = (_update_cb)md5_update;
        digest->_final = (_final_cb)md5_final;
        break;
    case DG_SHA1:
        digest->block_lens = SHA1_BLOCK_SIZE;
        digest->_init = (_init_cb)sha1_init;
        digest->_update = (_update_cb)sha1_update;
        digest->_final = (_final_cb)sha1_final;
        break;
    case DG_SHA256:
        digest->block_lens = SHA256_BLOCK_SIZE;
        digest->_init = (_init_cb)sha256_init;
        digest->_update = (_update_cb)sha256_update;
        digest->_final = (_final_cb)sha256_final;
        break;
    case DG_SHA512:
        digest->block_lens = SHA512_BLOCK_SIZE;
        digest->_init = (_init_cb)sha512_init;
        digest->_update = (_update_cb)sha512_update;
        digest->_final = (_final_cb)sha512_final;
        break;
    default:
        ASSERTAB(0, "unknow digest type.");
        break;
    }
    digest_reset(digest);
}
void digest_free(digest_ctx *digest) {
    secure_zero(digest, sizeof(digest_ctx));
}
size_t digest_size(digest_ctx *digest) {
    return digest->block_lens;
}
void digest_update(digest_ctx *digest, const void *data, size_t lens) {
    digest->_update(&digest->eng_ctx, data, lens);
}
// 各引擎的 _final 末尾都会 secure_zero 掉自己的 ctx（擦除中间状态），不重建 IV 的话
// 第二次 final 就是"拿全零 IV 算空消息"，返回一个与输入无关的常量。故这里出完摘要即复位
size_t digest_final(digest_ctx *digest, char *hash) {
    digest->_final(&digest->eng_ctx, hash);
    digest_reset(digest);
    return digest->block_lens;
}
void digest_reset(digest_ctx *digest) {
    digest->_init(&digest->eng_ctx);
}
