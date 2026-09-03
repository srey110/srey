#include "crypt/digest.h"

// 挂表用的薄封装。直接把 md5_init 一类转成 _init_cb 是不兼容函数指针转换(C99 6.3.2.3p8),
// -fsanitize=function / CFI 下一调用就中止
#define DG_THUNK(nm) \
    static void _dg_##nm##_init(void *ctx) { nm##_init((nm##_ctx *)ctx); } \
    static void _dg_##nm##_update(void *ctx, const void *data, size_t lens) { \
        nm##_update((nm##_ctx *)ctx, data, lens); \
    } \
    static void _dg_##nm##_final(void *ctx, char *hash) { nm##_final((nm##_ctx *)ctx, hash); }

DG_THUNK(md2)
DG_THUNK(md4)
DG_THUNK(md5)
DG_THUNK(sha1)
DG_THUNK(sha256)
DG_THUNK(sha512)
// 每种摘要算法的一行参数，下标即 digest_type 的值。新增算法只加一行，放哪都行——
// 指定了下标，往枚举中间插值也不会整体错位；没填的下标整行为零，由 digest_init 挡下
static const dg_attr _dg_tbl[] = {
    [DG_MD2]    = { MD2_BLOCK_SIZE,    MD2_KEY_BLOCK,    sizeof(md2_ctx),    _dg_md2_init,    _dg_md2_update,    _dg_md2_final    },
    [DG_MD4]    = { MD4_BLOCK_SIZE,    MD4_KEY_BLOCK,    sizeof(md4_ctx),    _dg_md4_init,    _dg_md4_update,    _dg_md4_final    },
    [DG_MD5]    = { MD5_BLOCK_SIZE,    MD5_KEY_BLOCK,    sizeof(md5_ctx),    _dg_md5_init,    _dg_md5_update,    _dg_md5_final    },
    [DG_SHA1]   = { SHA1_BLOCK_SIZE,   SHA1_KEY_BLOCK,   sizeof(sha1_ctx),   _dg_sha1_init,   _dg_sha1_update,   _dg_sha1_final   },
    [DG_SHA256] = { SHA256_BLOCK_SIZE, SHA256_KEY_BLOCK, sizeof(sha256_ctx), _dg_sha256_init, _dg_sha256_update, _dg_sha256_final },
    [DG_SHA512] = { SHA512_BLOCK_SIZE, SHA512_KEY_BLOCK, sizeof(sha512_ctx), _dg_sha512_init, _dg_sha512_update, _dg_sha512_final }
};

void digest_init(digest_ctx *digest, digest_type dtype) {
    ASSERTAB((size_t)dtype < ARRAY_SIZE(_dg_tbl)
        && NULL != _dg_tbl[dtype]._init, "unknow digest type.");
    digest->attr = _dg_tbl[dtype];
    digest_reset(digest);
}
void digest_free(digest_ctx *digest) {
    secure_zero(digest, sizeof(digest_ctx));
}
size_t digest_size(digest_ctx *digest) {
    return digest->attr.block_lens;
}
void digest_update(digest_ctx *digest, const void *data, size_t lens) {
    digest->attr._update(&digest->eng_ctx, data, lens);
}
// 各引擎的 _final 末尾都会 secure_zero 掉自己的 ctx（擦除中间状态），不重建 IV 的话
// 第二次 final 就是"拿全零 IV 算空消息"，返回一个与输入无关的常量。故这里出完摘要即复位
size_t digest_final(digest_ctx *digest, char *hash) {
    digest->attr._final(&digest->eng_ctx, hash);
    digest_reset(digest);
    return digest->attr.block_lens;
}
void digest_reset(digest_ctx *digest) {
    digest->attr._init(&digest->eng_ctx);
}
