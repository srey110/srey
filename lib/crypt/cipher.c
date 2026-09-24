#include "crypt/cipher.h"
#include "crypt/padding.h"

// 挂表用的薄封装,同 digest.c 的 DG_THUNK:直接转 _cipher_cb 是不兼容函数指针转换
static char *_cipher_aes(void *ctx, const void *data) {
    return aes_crypt((aes_ctx *)ctx, data);
}
static char *_cipher_des(void *ctx, const void *data) {
    return des_crypt((des_ctx *)ctx, data);
}
void cipher_init(cipher_ctx *cipher, engine_type engine, cipher_model model,
    const char *key, size_t klens, int32_t keybits, int32_t encrypt) {
    ASSERTAB(model >= ECB && model <= CTR, "unknow cipher model.");
    ZERO(cipher, sizeof(cipher_ctx));
    cipher->encrypt = encrypt;
    cipher->model = model;
    cipher->padding = NoPadding;
    int32_t fwdkey = (cipher->encrypt
        || CFB == cipher->model
        || OFB == cipher->model
        || CTR == cipher->model);
    switch (engine) {
    case AES:
        cipher->block_lens = AES_BLOCK_SIZE;
        cipher->_cipher = _cipher_aes;
        aes_init(&cipher->eng_ctx.aes, key, klens, keybits, fwdkey);
        break;
    case DES:
    case DES3:
        cipher->block_lens = DES_BLOCK_SIZE;
        cipher->_cipher = _cipher_des;
        des_init(&cipher->eng_ctx.des, key, klens, DES3 == engine, fwdkey);
        break;
    default:
        ASSERTAB(0, "unknow cipher engine.");
        break;
    }
}
void cipher_free(cipher_ctx *cipher) {
    secure_zero(cipher, sizeof(cipher_ctx));
}
size_t cipher_size(cipher_ctx *cipher) {
    return cipher->block_lens;
}
void cipher_padding(cipher_ctx *cipher, padding_model padding) {
    ASSERTAB((uint32_t)padding <= (uint32_t)ANSIX923, "unknow padding model.");
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
static inline const void *_cipher_process_data(cipher_ctx *cipher, const void *data, size_t lens, size_t *size) {
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
        //此处 lens < block_lens 必然装得下; 判返回值是为了不把未写过的 pd_data 当密文用
        if (ERR_OK != _padding_data(cipher->padding, data, lens, cipher->pd_data, cipher->block_lens)) {
            return NULL;
        }
        *size = cipher->block_lens;
        return (const void *)cipher->pd_data;
    }
    *size = lens;
    return data;
}
// 将 data 与 xorbuf 异或，结果存入 out（可与 data 重合）；按 4 字节一组做，不足一组的尾部逐字节
static void _cipher_xor_data(uint8_t *out, const uint8_t *data, const uint8_t *xorbuf, size_t lens) {
    uint32_t a, b;
    size_t i = 0;
    for (; i + sizeof(a) <= lens; i += sizeof(a)) {
        memcpy(&a, data + i, sizeof(a));
        memcpy(&b, xorbuf + i, sizeof(b));
        a ^= b;
        memcpy(out + i, &a, sizeof(a));
    }
    for (; i < lens; i++) {
        out[i] = data[i] ^ xorbuf[i];
    }
}
// CTR 模式下将整个 IV 块作为大端计数器自增
static void _cipher_inc_iv(uint8_t *iv, int32_t block_lens) {
    for (int32_t idx = block_lens - 1; idx >= 0; idx--) {
        iv[idx]++;
        if (0 != iv[idx]) {
            break;
        }
    }
}
// ECB 模式：直接对数据块进行加解密
static inline void *_cipher_ecb_model(cipher_ctx *cipher, const void *data) {
    return (void *)cipher->_cipher(&cipher->eng_ctx, data);
}
// 以下 CBC/CFB/OFB/CTR 的 blk 为分组长度
// CBC 模式：加密时先与 IV 异或再加密，解密时先解密再与 IV 异或
static inline void *_cipher_cbc_model(cipher_ctx *cipher, const void *data, size_t blk) {
    if (cipher->encrypt) {
        _cipher_xor_data(cipher->xor_data, data, cipher->cur_iv, blk);
        void *en = (void *)cipher->_cipher(&cipher->eng_ctx, cipher->xor_data);
        memcpy(cipher->cur_iv, en, blk);
        return en;
    }
    void *de = (void *)cipher->_cipher(&cipher->eng_ctx, data);
    _cipher_xor_data(cipher->xor_data, de, cipher->cur_iv, blk);
    memcpy(cipher->cur_iv, data, blk);
    return (void *)cipher->xor_data;
}
// CFB 模式：加密 IV 得到密钥流，与数据异或；移位寄存器更新为密文块
static inline void *_cipher_cfb_model(cipher_ctx *cipher, const void *data, size_t lens, size_t blk) {
    void *en = (void *)cipher->_cipher(&cipher->eng_ctx, cipher->cur_iv);
    _cipher_xor_data(cipher->xor_data, data, en, lens);
    if (lens == blk) {
        if (cipher->encrypt) {
            memcpy(cipher->cur_iv, cipher->xor_data, blk);
        } else {
            memcpy(cipher->cur_iv, data, blk);
        }
    }
    return (void *)cipher->xor_data;
}
// OFB 模式：将数据与加密后的 IV 异或，加解密共用同一逻辑
static inline void *_cipher_ofb_model(cipher_ctx *cipher, const void *data, size_t lens, size_t blk) {
    void *en = (void *)cipher->_cipher(&cipher->eng_ctx, cipher->cur_iv);
    _cipher_xor_data(cipher->xor_data, data, en, lens);
    memcpy(cipher->cur_iv, en, blk);
    return (void *)cipher->xor_data;
}
// CTR 模式：加密计数器后与数据异或，并自增计数器
static inline void *_cipher_ctr_model(cipher_ctx *cipher, const void *data, size_t lens, size_t blk) {
    void *en = (void *)cipher->_cipher(&cipher->eng_ctx, cipher->cur_iv);
    _cipher_xor_data(cipher->xor_data, data, en, lens);
    _cipher_inc_iv(cipher->cur_iv, (int32_t)blk);
    return (void *)cipher->xor_data;
}
// 连续处理 lens 字节（16 的正整数倍）的整分组：整分组无需预处理必然成功，模式判断提到循环外；
// 只走 AES，output 与 data 重合（原地加解密）也安全
static inline void _cipher_full_blocks(cipher_ctx *cipher, const uint8_t *data, size_t lens, uint8_t *output) {
    const size_t blk = AES_BLOCK_SIZE;
    const uint8_t *prev, *en;
    uint64_t iv[2], cur[2], pt[2];
    size_t i;
    switch (cipher->model) {
    case ECB:
        for (i = 0; i < lens; i += blk) {
            memcpy(output + i, _cipher_ecb_model(cipher, data + i), blk);
        }
        break;
    case CBC:
        if (cipher->encrypt) {
            //上一组密文直接取引擎输出，整段做完再写回 cur_iv
            prev = cipher->cur_iv;
            for (i = 0; i < lens; i += blk) {
                _cipher_xor_data(cipher->xor_data, data + i, prev, blk);
                prev = (const uint8_t *)cipher->_cipher(&cipher->eng_ctx, cipher->xor_data);
                memcpy(output + i, prev, blk);
            }
            memcpy(cipher->cur_iv, prev, blk);
        } else {
            //本组密文先存进 cur 再写输出，原地解密时下一组的 IV 不会被覆盖
            memcpy(iv, cipher->cur_iv, sizeof(iv));
            for (i = 0; i < lens; i += blk) {
                memcpy(cur, data + i, sizeof(cur));
                memcpy(pt, cipher->_cipher(&cipher->eng_ctx, data + i), sizeof(pt));
                pt[0] ^= iv[0];
                pt[1] ^= iv[1];
                memcpy(output + i, pt, sizeof(pt));
                iv[0] = cur[0];
                iv[1] = cur[1];
            }
            memcpy(cipher->cur_iv, iv, sizeof(iv));
            secure_zero(pt, sizeof(pt));// 末块明文别留在栈上
        }
        break;
    case CFB:
        if (cipher->encrypt) {
            //移位寄存器即上一组密文，直接取 output 里刚写的那组，整段做完再写回 cur_iv
            prev = cipher->cur_iv;
            for (i = 0; i < lens; i += blk) {
                en = (const uint8_t *)cipher->_cipher(&cipher->eng_ctx, prev);
                _cipher_xor_data(output + i, data + i, en, blk);
                prev = output + i;
            }
            memcpy(cipher->cur_iv, prev, blk);
        } else {
            //本组密文先存进 cur_iv 再写输出，原地解密时移位寄存器不会被覆盖
            for (i = 0; i < lens; i += blk) {
                en = (const uint8_t *)cipher->_cipher(&cipher->eng_ctx, cipher->cur_iv);
                memcpy(cipher->cur_iv, data + i, blk);
                _cipher_xor_data(output + i, data + i, en, blk);
            }
        }
        break;
    case OFB:
        //密钥流直接在引擎输出缓冲上迭代（引擎先读完输入再写输出），整段做完再写回 cur_iv
        prev = cipher->cur_iv;
        for (i = 0; i < lens; i += blk) {
            prev = (const uint8_t *)cipher->_cipher(&cipher->eng_ctx, prev);
            _cipher_xor_data(output + i, data + i, prev, blk);
        }
        memcpy(cipher->cur_iv, prev, blk);
        break;
    case CTR:
        for (i = 0; i < lens; i += blk) {
            en = (const uint8_t *)cipher->_cipher(&cipher->eng_ctx, cipher->cur_iv);
            _cipher_xor_data(output + i, data + i, en, blk);
            _cipher_inc_iv(cipher->cur_iv, (int32_t)blk);
        }
        break;
    }
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
        rtn = _cipher_cbc_model(cipher, input, cipher->block_lens);
        break;
    case CFB:
        rtn = _cipher_cfb_model(cipher, input, lens, cipher->block_lens);
        break;
    case OFB:
        rtn = _cipher_ofb_model(cipher, input, lens, cipher->block_lens);
        break;
    case CTR:
        rtn = _cipher_ctr_model(cipher, input, lens, cipher->block_lens);
        break;
    default:
        break;
    }
    return rtn;
}
int32_t cipher_dofinal(cipher_ctx *cipher, const void *data, size_t lens, char *output, size_t *outlens) {
    void *buf;
    size_t enlens, size = 0;
    *outlens = 0;
    cipher_reset(cipher);
    //AES 的整分组成批处理；DES 的分组与末尾不足一组的部分照旧逐组经 cipher_block 校验与填充
    if (AES_BLOCK_SIZE == cipher->block_lens && lens >= AES_BLOCK_SIZE) {
        size = lens - lens % AES_BLOCK_SIZE;
        _cipher_full_blocks(cipher, data, size, (uint8_t *)output);
    }
    for (size_t i = size; i < lens; i += cipher->block_lens) {
        enlens = (i + cipher->block_lens > lens ? lens - i : cipher->block_lens);
        buf = cipher_block(cipher, (const char *)data + i, enlens, &enlens);
        if (NULL == buf) {
            goto fail;
        }
        memcpy(output + size, buf, enlens);
        size += enlens;
    }
    if (PKCS57 == cipher->padding
        || ISO10126 == cipher->padding
        || ANSIX923 == cipher->padding) {
        if (cipher->encrypt) {
            if (0 == lens % cipher->block_lens) {
                //dlens=0 必然装得下; 同上, 判返回值避免把未写过的 pd_data 当整填充块发出去
                if (ERR_OK != _padding_data(cipher->padding, NULL, 0, cipher->pd_data, cipher->block_lens)) {
                    goto fail;
                }
                buf = cipher_block(cipher, cipher->pd_data, cipher->block_lens, &enlens);
                memcpy(output + size, buf, enlens);
                size += enlens;
            }
        } else {
            //解密路径：最后一个分组含填充字节，校验后剥离
            //size < block_lens（含 size==0）属解密失败，避免 output[size-1] 下溢越界
            if (size < cipher->block_lens) {
                goto fail;
            }
            uint8_t pad = (uint8_t)output[size - 1];
            if (ISO10126 == cipher->padding) {
                //ISO 10126 前 N-1 字节是随机数,除长度字节的范围外无从校验 —— 这一档做不成
                //常数时间,成败只由 pad 决定
                if (pad < 1 || pad > cipher->block_lens) {
                    goto fail;
                }
            } else {
                //PKCS#7 / ANSI X.923 常数时间校验:循环长度固定 [1, blk),用 mask 屏蔽非填充区;
                //期望值 PKCS#7 全部 == pad、ANSI X.923 前 N-1 字节 == 0(末尾那个即 j==0,与 pad 等价故不入循环);
                //bad 用 volatile 防编译器把累加优化成提前退出
                size_t blk = cipher->block_lens;
                uint8_t expected = (PKCS57 == cipher->padding) ? pad : (uint8_t)0;
                volatile uint8_t bad = 0;
                uint8_t b, mask;
                uint32_t lt;
                //范围检查折进 bad 而非提前 return:两条失败路径耗时不同就等于把"长度字节是否合法"漏出去。
                //pad==0 或 pad>blk 时 oor 为 1;循环只读 output[size-1-j] 且 j<blk<=size,pad 再离谱也不越界
                uint32_t oor = (((uint32_t)pad - 1u) | ((uint32_t)blk - (uint32_t)pad)) >> 31;
                bad |= (uint8_t)(0u - oor);
                for (size_t j = 1; j < blk; j++) {
                    b = (uint8_t)output[size - 1 - j];
                    //lt = 1 当 j<pad（位于填充区），否则 0
                    lt = ((uint32_t)j - (uint32_t)pad) >> 31;
                    mask = (uint8_t)(0u - lt);
                    bad |= mask & (b ^ expected);
                }
                if (0 != bad) {
                    goto fail;
                }
            }
            size -= pad;
            secure_zero(output + size, pad);
        }
    }
    *outlens = size;
    return ERR_OK;
fail:
    // 所有失败路径共用: 抹掉已写入的部分, 不把半截明文留在调用方缓冲里
    secure_zero(output, size);
    return ERR_FAILED;
}
