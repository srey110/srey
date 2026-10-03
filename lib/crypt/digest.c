#include "crypt/digest.h"

// 挂表用的薄封装。直接把 md5_init 一类转成 _init_cb 是不兼容函数指针转换(C99 6.3.2.3p8),
// -fsanitize=function / CFI 下一调用就中止
#define DG_THUNK(nm) \
    static void _dg_##nm##_init(void *ctx) { nm##_init((nm##_ctx *)ctx); } \
    static void _dg_##nm##_update(void *ctx, const void *data, size_t lens) { \
        nm##_update((nm##_ctx *)ctx, data, lens); \
    } \
    static void _dg_##nm##_final(void *ctx, char *hash) { nm##_final((nm##_ctx *)ctx, hash); }
// xxhash 的 init 带 seed、digest 返回整数，套不进 DG_THUNK；seed 与输出字节序的约定见 digest.h
#define DG_XXH_THUNK(nm, wr) \
    static void _dg_##nm##_init(void *ctx) { nm##_init((nm##_ctx *)ctx, 0); } \
    static void _dg_##nm##_update(void *ctx, const void *data, size_t lens) { \
        nm##_update((nm##_ctx *)ctx, data, lens); \
    } \
    static void _dg_##nm##_final(void *ctx, char *hash) { wr(hash, nm##_digest((nm##_ctx *)ctx)); }
// 状态字与填充长度字段的字节序（1 小端），DG_STATE_THUNK 与下面的表共用这一处
#define DG_LE_md4 1
#define DG_LE_md5 1
#define DG_LE_sha1 0
#define DG_LE_sha256 0
#define DG_LE_sha512 0
// 只导出 state 字段，所以只适用于"状态即输出"的 MD 结构引擎
#define DG_STATE_THUNK(nm, bits) \
    static void _dg_##nm##_state(void *ctx, char *hash) { \
        nm##_ctx *eng = (nm##_ctx *)ctx; \
        size_t i; \
        for (i = 0; i < ARRAY_SIZE(eng->state); i++) { \
            DG_LE_##nm ? write_le##bits(hash + i * sizeof(eng->state[0]), eng->state[i]) \
                : write_be##bits(hash + i * sizeof(eng->state[0]), eng->state[i]); \
        } \
    }

DG_THUNK(md2)
DG_THUNK(md4)
DG_THUNK(md5)
DG_THUNK(sha1)
DG_THUNK(sha256)
DG_THUNK(sha512)
DG_XXH_THUNK(xxh32, write_be32)
DG_XXH_THUNK(xxh64, write_be64)
DG_STATE_THUNK(md4, 32)
DG_STATE_THUNK(md5, 32)
DG_STATE_THUNK(sha1, 32)
DG_STATE_THUNK(sha256, 32)
DG_STATE_THUNK(sha512, 64)
// 每种摘要算法的一行参数，下标即 digest_type 的值。新增算法只加一行，放哪都行——
// 指定了下标，往枚举中间插值也不会整体错位；没填的下标整行为零，由 digest_init 挡下
static const dg_attr _dg_tbl[] = {
    [DG_MD2]    = { MD2_BLOCK_SIZE,    MD2_KEY_BLOCK,    sizeof(md2_ctx),    _dg_md2_init,    _dg_md2_update,    _dg_md2_final,    NULL,             0 },
    [DG_MD4]    = { MD4_BLOCK_SIZE,    MD4_KEY_BLOCK,    sizeof(md4_ctx),    _dg_md4_init,    _dg_md4_update,    _dg_md4_final,    _dg_md4_state,    DG_LE_md4 },
    [DG_MD5]    = { MD5_BLOCK_SIZE,    MD5_KEY_BLOCK,    sizeof(md5_ctx),    _dg_md5_init,    _dg_md5_update,    _dg_md5_final,    _dg_md5_state,    DG_LE_md5 },
    [DG_SHA1]   = { SHA1_BLOCK_SIZE,   SHA1_KEY_BLOCK,   sizeof(sha1_ctx),   _dg_sha1_init,   _dg_sha1_update,   _dg_sha1_final,   _dg_sha1_state,   DG_LE_sha1 },
    [DG_SHA256] = { SHA256_BLOCK_SIZE, SHA256_KEY_BLOCK, sizeof(sha256_ctx), _dg_sha256_init, _dg_sha256_update, _dg_sha256_final, _dg_sha256_state, DG_LE_sha256 },
    [DG_SHA512] = { SHA512_BLOCK_SIZE, SHA512_KEY_BLOCK, sizeof(sha512_ctx), _dg_sha512_init, _dg_sha512_update, _dg_sha512_final, _dg_sha512_state, DG_LE_sha512 },
    [DG_XXH32]  = { XXH32_BLOCK_SIZE,  0,                sizeof(xxh32_ctx),  _dg_xxh32_init,  _dg_xxh32_update,  _dg_xxh32_final,  NULL,             0 },
    [DG_XXH64]  = { XXH64_BLOCK_SIZE,  0,                sizeof(xxh64_ctx),  _dg_xxh64_init,  _dg_xxh64_update,  _dg_xxh64_final,  NULL,             0 }
};

void digest_init(digest_ctx *digest, digest_type dtype) {
    ASSERTAB((size_t)dtype < ARRAY_SIZE(_dg_tbl)
        && NULL != _dg_tbl[dtype]._init, "unknow digest type.");
    digest->attr = &_dg_tbl[dtype];
    digest_reset(digest);
}
void digest_free(digest_ctx *digest) {
    secure_zero(digest, sizeof(digest_ctx));
}
size_t digest_size(digest_ctx *digest) {
    return digest->attr->block_lens;
}
void digest_update(digest_ctx *digest, const void *data, size_t lens) {
    digest->attr->_update(&digest->eng_ctx, data, lens);
}
// 密码学引擎的 _final 末尾都会 secure_zero 掉自己的 ctx（擦除中间状态），不重建 IV 的话
// 第二次 final 就是"拿全零 IV 算空消息"，返回一个与输入无关的常量。故这里出完摘要即复位
size_t digest_final(digest_ctx *digest, char *hash) {
    digest->attr->_final(&digest->eng_ctx, hash);
    digest_reset(digest);
    return digest->attr->block_lens;
}
void digest_reset(digest_ctx *digest) {
    digest->attr->_init(&digest->eng_ctx);
}
