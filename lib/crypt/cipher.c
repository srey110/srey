#include "crypt/cipher.h"
#include "crypt/padding.h"
#include "utils/utils.h"

void cipher_init(cipher_ctx *cipher, engine_type engine, cipher_model model,
    const char *key, size_t klens, int32_t keybits, int32_t encrypt) {
    cipher->encrypt = encrypt;
    cipher->model = model;
    cipher->padding = NoPadding;
    if (AES == engine) {
        cipher->block_lens = AES_BLOCK_SIZE;
        cipher->cur_ctx = &cipher->eng_ctx.aes;
        cipher->_cipher = (_cipher_cb)aes_crypt;
        if (cipher->encrypt
            || CFB == cipher->model
            || OFB == cipher->model
            || CTR == cipher->model) {
            aes_init(cipher->cur_ctx, key, klens, keybits, 1);
        } else {
            aes_init(cipher->cur_ctx, key, klens, keybits, 0);
        }
        return;
    }
    cipher->block_lens = DES_BLOCK_SIZE;
    cipher->cur_ctx = &cipher->eng_ctx.des;
    cipher->_cipher = (_cipher_cb)des_crypt;
    if (cipher->encrypt
        || CFB == cipher->model
        || OFB == cipher->model
        || CTR == cipher->model) {
        des_init(cipher->cur_ctx, key, klens, DES3 == engine, 1);
    } else {
        des_init(cipher->cur_ctx, key, klens, DES3 == engine, 0);
    }
}
void cipher_free(cipher_ctx *cipher) {
    secure_zero(cipher, sizeof(cipher_ctx));
}
size_t cipher_size(cipher_ctx *cipher) {
    return cipher->block_lens;
}
void cipher_padding(cipher_ctx *cipher, padding_model padding) {
    cipher->padding = padding;
}
void cipher_iv(cipher_ctx *cipher, const char *iv, size_t ilens) {
    if (ECB == cipher->model) {
        return;
    }
    uint8_t *pdiv = _padding_key(iv, ilens, cipher->cur_iv, cipher->block_lens);
    memcpy(cipher->iv, pdiv, cipher->block_lens);
    cipher_reset(cipher);
}
void cipher_reset(cipher_ctx *cipher) {
    if (ECB != cipher->model) {
        memcpy(cipher->cur_iv, cipher->iv, cipher->block_lens);
    }
}
// 预处理待加解密数据：校验长度合法性，必要时执行填充，返回实际处理指针
static const void *_cipher_process_data(cipher_ctx *cipher, const void *data, size_t lens, size_t *size) {
    if (lens > cipher->block_lens) {
        return NULL;
    }
    //解密
    if (!cipher->encrypt) {
        if (lens != cipher->block_lens) {
            if (ECB == cipher->model
                || CBC == cipher->model) {
                return NULL;
            }
            if (NoPadding != cipher->padding) {
                return NULL;
            }
        }
        *size = lens;
        return data;
    }
    //加密 无填充
    if (NoPadding == cipher->padding) {
        if (lens != cipher->block_lens
            && (ECB == cipher->model || CBC == cipher->model)) {
            return NULL;
        }
        *size = lens;
        return data;
    }
    //填充
    if (lens < cipher->block_lens) {
        _padding_data(cipher->padding, data, lens, cipher->pd_data, cipher->block_lens);
        *size = cipher->block_lens;
        return (const void *)cipher->pd_data;
    }
    *size = lens;
    return data;
}
// 将 data 与 xorbuf 按字节异或，结果存入 cipher->xor_data
static void _cipher_xor_data(cipher_ctx *cipher, const uint8_t *data, const uint8_t *xorbuf, size_t lens) {
    for (size_t i = 0; i < lens; i++) {
        cipher->xor_data[i] = data[i] ^ xorbuf[i];
    }
}
// CTR 模式下自增 IV 计数器，counter_size 为计数器字节数
static void _cipher_inc_iv(uint8_t *iv, int32_t block_lens, int32_t counter_size) {
    int32_t nonce_idx = block_lens - counter_size;
    for (int32_t idx = block_lens - 1; idx >= nonce_idx; idx--) {
        iv[idx]++;
        if (0 != iv[idx]
            || idx == nonce_idx) {
            break;
        }
    }
}
// ECB 模式：直接对数据块进行加解密
static inline void *_cipher_ecb_model(cipher_ctx *cipher, const void *data) {
    return (void *)cipher->_cipher(cipher->cur_ctx, data);
}
// CBC 模式：加密时先与 IV 异或再加密，解密时先解密再与 IV 异或
static inline void *_cipher_cbc_model(cipher_ctx *cipher, const void *data) {
    if (cipher->encrypt) {
        _cipher_xor_data(cipher, data, cipher->cur_iv, cipher->block_lens);
        void *en = (void *)cipher->_cipher(cipher->cur_ctx, cipher->xor_data);
        memcpy(cipher->cur_iv, en, cipher->block_lens);
        return en;
    }
    void *de = (void *)cipher->_cipher(cipher->cur_ctx, data);
    _cipher_xor_data(cipher, de, cipher->cur_iv, cipher->block_lens);
    memcpy(cipher->cur_iv, data, cipher->block_lens);
    return (void *)cipher->xor_data;
}
// CFB 模式：加密 IV 得到密钥流，与数据异或；移位寄存器更新为密文块
static inline void *_cipher_cfb_model(cipher_ctx *cipher, const void *data, size_t lens) {
    void *en = (void *)cipher->_cipher(cipher->cur_ctx, cipher->cur_iv);
    _cipher_xor_data(cipher, data, en, lens);
    if (lens == cipher->block_lens) {
        if (cipher->encrypt) {
            memcpy(cipher->cur_iv, cipher->xor_data, cipher->block_lens);
        } else {
            memcpy(cipher->cur_iv, data, cipher->block_lens);
        }
    }
    return (void *)cipher->xor_data;
}
// OFB 模式：将数据与加密后的 IV 异或，加解密共用同一逻辑
static inline void *_cipher_ofb_model(cipher_ctx *cipher, const void *data, size_t lens) {
    void *en = (void *)cipher->_cipher(cipher->cur_ctx, cipher->cur_iv);
    _cipher_xor_data(cipher, data, en, lens);
    memcpy(cipher->cur_iv, en, cipher->block_lens);
    return (void *)cipher->xor_data;
}
// CTR 模式：加密计数器后与数据异或，并自增计数器
static inline void *_cipher_ctr_model(cipher_ctx *cipher, const void *data, size_t lens) {
    void *en = (void *)cipher->_cipher(cipher->cur_ctx, cipher->cur_iv);
    _cipher_xor_data(cipher, data, en, lens);
    _cipher_inc_iv(cipher->cur_iv, (int32_t)cipher->block_lens, (int32_t)(cipher->block_lens));
    return (void *)cipher->xor_data;
}
void *cipher_block(cipher_ctx *cipher, const void *data, size_t lens, size_t *size) {
    const void *input = _cipher_process_data(cipher, data, lens, &lens);
    if (NULL == input) {
        return NULL;
    }
    SET_PTR(size, lens);
    void *rtn = NULL;
    switch (cipher->model) {
    case ECB:
        rtn = _cipher_ecb_model(cipher, input);
        break;
    case CBC:
        rtn = _cipher_cbc_model(cipher, input);
        break;
    case CFB:
        rtn = _cipher_cfb_model(cipher, input, lens);
        break;
    case OFB:
        rtn = _cipher_ofb_model(cipher, input, lens);
        break;
    case CTR:
        rtn = _cipher_ctr_model(cipher, input, lens);
        break;
    default:
        break;
    }
    return rtn;
}
size_t cipher_dofinal(cipher_ctx *cipher, const void *data, size_t lens, char *output) {
    void *buf;
    size_t enlens, size = 0;
    cipher_reset(cipher);
    for (size_t i = 0; i < lens; i += cipher->block_lens) {
        enlens = (i + cipher->block_lens > lens ? lens - i : cipher->block_lens);
        buf = cipher_block(cipher, (const char *)data + i, enlens, &enlens);
        if (NULL == buf) {
            return size;
        }
        memcpy(output + size, buf, enlens);
        size += enlens;
    }
    if (PKCS57 == cipher->padding
        || ISO10126 == cipher->padding
        || ANSIX923 == cipher->padding) {
        if (cipher->encrypt) {
            if (0 == lens % cipher->block_lens) {
                _padding_data(cipher->padding, NULL, 0, cipher->pd_data, cipher->block_lens);
                buf = cipher_block(cipher, cipher->pd_data, cipher->block_lens, &enlens);
                //合法初始化下不会返回 NULL（_cipher_process_data 走 line 101 直返；model 必为枚举内值）；
                //此处与 line 205 同款防御 NULL，避免未来扩展 model 时静默段错误。
                if (NULL == buf) {
                    return size;
                }
                memcpy(output + size, buf, enlens);
                size += enlens;
            }
        } else {
            //解密路径：最后一个分组含填充字节，校验后剥离
            //size < block_lens（含 size==0）属解密失败，避免 output[size-1] 下溢越界
            if (size < cipher->block_lens) {
                secure_zero(output, size);
                return 0;
            }
            uint8_t pad = (uint8_t)output[size - 1];
            if (pad < 1 || pad > cipher->block_lens) {
                secure_zero(output, size);
                return 0;
            }
            if (ISO10126 == cipher->padding) {
                //ISO 10126 前 N-1 字节为随机数无法校验，长度字节已由上界检查覆盖
                size -= pad;
                secure_zero(output + size, pad);
            } else {
                //PKCS#7 / ANSI X.923 常数时间校验：
                //  - 期望值：PKCS#7 全部 == pad；ANSI X.923 前 N-1 字节 == 0、末尾 == pad
                //  - 循环范围固定为 [1, block_lens)，用 mask 屏蔽非填充区
                //  - 避免 padding oracle 计时侧信道（循环长度不依赖 pad）
                //  - bad 用 volatile 修饰，与 utils/ct_memcmp 同款语义，防止编译器把累加优化成提前退出
                //注：j==0 即长度字节本身，已与 expected 等价（PKCS#7 等于 pad；
                //ANSI X.923 末尾也等于 pad，两者末尾期望均为 pad），无需在循环中重复校验
                //本函数无法直接复用 ct_memcmp：padding 校验需对非填充区做 mask 屏蔽，
                //而 ct_memcmp 是双 buffer 等长比较，不带位置条件
                size_t blk = cipher->block_lens;
                uint8_t expected = (PKCS57 == cipher->padding) ? pad : (uint8_t)0;
                volatile uint8_t bad = 0;
                uint8_t b, mask;
                uint32_t lt;
                for (size_t j = 1; j < blk; j++) {
                    b = (uint8_t)output[size - 1 - j];
                    //lt = 1 当 j<pad（位于填充区），否则 0
                    lt = ((uint32_t)j - (uint32_t)pad) >> 31;
                    mask = (uint8_t)(0u - lt);
                    bad |= mask & (b ^ expected);
                }
                if (0 != bad) {
                    secure_zero(output, size);
                    return 0;
                }
                size -= pad;
                secure_zero(output + size, pad);
            }
        }
    }
    return size;
}
