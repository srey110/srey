#include "crypt/digest.h"

// 每种摘要算法的一行参数，下标即 digest_type 的值。新增算法只加一行，放哪都行——
// 指定了下标，往枚举中间插值也不会整体错位；没填的下标整行为零，由 digest_init 挡下
typedef struct dg_entry {
    size_t block_lens;
    size_t key_block;
    size_t eng_lens;
    _init_cb _init;
    _update_cb _update;
    _final_cb _final;
}dg_entry;

static const dg_entry _dg_tbl[] = {
    [DG_MD2]    = { MD2_BLOCK_SIZE,    MD2_KEY_BLOCK,    sizeof(md2_ctx),    (_init_cb)md2_init,    (_update_cb)md2_update,    (_final_cb)md2_final    },
    [DG_MD4]    = { MD4_BLOCK_SIZE,    MD4_KEY_BLOCK,    sizeof(md4_ctx),    (_init_cb)md4_init,    (_update_cb)md4_update,    (_final_cb)md4_final    },
    [DG_MD5]    = { MD5_BLOCK_SIZE,    MD5_KEY_BLOCK,    sizeof(md5_ctx),    (_init_cb)md5_init,    (_update_cb)md5_update,    (_final_cb)md5_final    },
    [DG_SHA1]   = { SHA1_BLOCK_SIZE,   SHA1_KEY_BLOCK,   sizeof(sha1_ctx),   (_init_cb)sha1_init,   (_update_cb)sha1_update,   (_final_cb)sha1_final   },
    [DG_SHA256] = { SHA256_BLOCK_SIZE, SHA256_KEY_BLOCK, sizeof(sha256_ctx), (_init_cb)sha256_init, (_update_cb)sha256_update, (_final_cb)sha256_final },
    [DG_SHA512] = { SHA512_BLOCK_SIZE, SHA512_KEY_BLOCK, sizeof(sha512_ctx), (_init_cb)sha512_init, (_update_cb)sha512_update, (_final_cb)sha512_final }
};

void digest_init(digest_ctx *digest, digest_type dtype) {
    ASSERTAB((size_t)dtype < ARRAY_SIZE(_dg_tbl)
        && NULL != _dg_tbl[dtype]._init, "unknow digest type.");
    const dg_entry *entry = &_dg_tbl[dtype];
    digest->block_lens = entry->block_lens;
    digest->key_block = entry->key_block;
    digest->eng_lens = entry->eng_lens;
    digest->_init = entry->_init;
    digest->_update = entry->_update;
    digest->_final = entry->_final;
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
