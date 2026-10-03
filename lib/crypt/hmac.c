#include "crypt/hmac.h"

/* RFC 2104：ipad/opad 长度等于压缩函数输入块大小 B，不是摘要输出长度。B 由 digest_init
 * 一并写进 digest_ctx.attr->key_block —— 算法属性只留一张表，新增算法漏填会被 digest_init 的 ASSERTAB 挡住。
 * HMAC_MAX_KEY_LENS 取所有支持算法中最大的 B，用于栈缓冲区声明。 */
#define HMAC_MAX_KEY_LENS 128

void hmac_init(hmac_ctx *hmac, digest_type dtype, const char *key, size_t klens) {
    digest_init(&hmac->inside, dtype);
    digest_init(&hmac->outside, dtype);
    digest_init(&hmac->inside_init, dtype);
    digest_init(&hmac->outside_init, dtype);
    size_t key_block = hmac->inside.attr->key_block;
    ASSERTAB(0 < key_block && key_block <= HMAC_MAX_KEY_LENS, "digest type not usable for hmac or key block exceeds stack buffer.");
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
// 不走 digest_final：它复位到算法 IV，而 HMAC 要的是 ipad/opad 吸收之后的状态，统一由 hmac_reset 恢复。
// 少这一步则第二次 final 变成"无密钥"的摘要，对任何密钥都得到同一个常量
size_t hmac_final(hmac_ctx *hmac, char *hash) {
    const dg_attr *attr = hmac->inside.attr;
    attr->_final(&hmac->inside.eng_ctx, hash);
    attr->_update(&hmac->outside.eng_ctx, hash, attr->block_lens);
    attr->_final(&hmac->outside.eng_ctx, hash);
    hmac_reset(hmac);
    return attr->block_lens;
}
// 按当前引擎的实际 ctx 大小拷，不按联合体整份：PBKDF2 每轮要拷两次，多拷的部分随迭代次数放大
void hmac_reset(hmac_ctx *hmac) {
    ASSERTAB(hmac->inside_init.attr->eng_lens <= sizeof(hmac->inside_init.eng_ctx)
             && hmac->outside_init.attr->eng_lens <= sizeof(hmac->outside_init.eng_ctx),
             "engine ctx larger than hmac buffer.");
    memcpy(&hmac->inside.eng_ctx, &hmac->inside_init.eng_ctx, hmac->inside_init.attr->eng_lens);
    memcpy(&hmac->outside.eng_ctx, &hmac->outside_init.eng_ctx, hmac->outside_init.attr->eng_lens);
}
// 定长消息"整块 + hs 字节"的末块：前 hs 字节留给每轮的 U，其余填充只写一次。
// 长度只写末 8 字节，sha512 的 16 字节长度字段高半恒为 0，由 memset 顺带清掉
static void _hmac_pad_tail(const dg_attr *attr, char *blk) {
    size_t hs = attr->block_lens, kb = attr->key_block;
    char *tail = blk + kb - sizeof(uint64_t);
    uint64_t nbits = (uint64_t)(kb + hs) * 8;
    blk[hs] = (char)0x80;
    memset(blk + hs + 1, 0, kb - hs - 1 - sizeof(uint64_t));
    attr->islittle ? write_le64(tail, nbits) : write_be64(tail, nbits);
}
// 从吸收完 ipad/opad 的状态出发压一块 blk，结果写回 blk 的前 hs 字节；绕开 _final 的填充与擦除
static void _hmac_block(digest_ctx *work, const digest_ctx *init, char *blk) {
    const dg_attr *attr = init->attr;
    memcpy(&work->eng_ctx, &init->eng_ctx, attr->eng_lens);
    attr->_update(&work->eng_ctx, blk, attr->key_block);
    attr->_state(&work->eng_ctx, blk);
}
size_t hmac_pbkdf2(hmac_ctx *hmac, const void *salt, size_t slens, int32_t iter, char *out) {
    static const char one[4] = { 0, 0, 0, 1 };
    const dg_attr *attr = hmac->inside.attr;
    size_t hs = attr->block_lens, j;
    char u[HMAC_MAX_KEY_LENS];
    int32_t i;
    hmac_update(hmac, salt, slens);
    hmac_update(hmac, one, sizeof(one));
    hmac_final(hmac, u);
    memcpy(out, u, hs);
    if (NULL != attr->_state) {
        _hmac_pad_tail(attr, u);
    }
    for (i = 1; i < iter; i++) {
        if (NULL != attr->_state) {
            _hmac_block(&hmac->inside, &hmac->inside_init, u);
            _hmac_block(&hmac->outside, &hmac->outside_init, u);
        } else {
            hmac_update(hmac, u, hs);
            hmac_final(hmac, u);
        }
        for (j = 0; j < hs; j++) {
            out[j] ^= u[j];
        }
    }
    // 快路径把中间状态留在了 inside/outside 里，复位同时覆盖掉它
    hmac_reset(hmac);
    secure_zero(u, sizeof(u));
    return hs;
}
