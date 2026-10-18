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
    cipher->ks_used = 0;
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
// CBC 模式：加密时先与 IV 异或再加密，解密时先解密再与 IV 异或；blk 为分组长度
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
// CFB / OFB / CTR：按字节续用本组密钥流 ks，用完一组才加密 cur_iv 生成下一组（OFB 随之把 cur_iv 换成 ks，
// CTR 计数器加一）；CFB 的移位寄存器逐字节换成密文，凑满一组正好是下一组的输入。lens 不超过分组长度
static inline void *_cipher_stream_model(cipher_ctx *cipher, const uint8_t *data, size_t lens) {
    const size_t blk = cipher->block_lens;
    size_t off = 0, n;
    while (off < lens) {
        if (0 == cipher->ks_used) {
            memcpy(cipher->ks, cipher->_cipher(&cipher->eng_ctx, cipher->cur_iv), blk);
            if (OFB == cipher->model) {
                memcpy(cipher->cur_iv, cipher->ks, blk);
            } else if (CTR == cipher->model) {
                _cipher_inc_iv(cipher->cur_iv, (int32_t)blk);
            }
        }
        n = blk - cipher->ks_used;
        if (n > lens - off) {
            n = lens - off;
        }
        //解密时密文先进寄存器，输入与输出同址也不会读到已解出的明文
        if (CFB == cipher->model && !cipher->encrypt) {
            memcpy(cipher->cur_iv + cipher->ks_used, data + off, n);
        }
        _cipher_xor_data(cipher->xor_data + off, data + off, cipher->ks + cipher->ks_used, n);
        if (CFB == cipher->model && cipher->encrypt) {
            memcpy(cipher->cur_iv + cipher->ks_used, cipher->xor_data + off, n);
        }
        cipher->ks_used += (uint32_t)n;
        if (blk == cipher->ks_used) {
            cipher->ks_used = 0;
        }
        off += n;
    }
    return (void *)cipher->xor_data;
}
// 连续处理 lens 字节（16 的正整数倍）的整分组：整分组无需预处理必然成功，模式判断提到循环外；
// 只走 AES，output 与 data 重合（原地加解密）也安全；进入时 ks_used 须为 0（dofinal 先 reset）
static inline void _cipher_full_blocks(cipher_ctx *cipher, const uint8_t *data, size_t lens, uint8_t *output) {
    const size_t blk = AES_BLOCK_SIZE;
    const uint8_t *prev, *en;
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
            uint64_t iv[2], cur[2], pt[2];
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
    switch (cipher->model) {
    case ECB:
        return _cipher_ecb_model(cipher, input);
    case CBC:
        return _cipher_cbc_model(cipher, input, cipher->block_lens);
    case CFB:
    case OFB:
    case CTR:
        return _cipher_stream_model(cipher, input, lens);
    }
    return NULL;// model 已由 cipher_init 限定，到不了这里；不写 default 让 -Wswitch 盯住新增模式
}
int32_t cipher_dofinal(cipher_ctx *cipher, const void *data, size_t lens, char *output, size_t *outlens) {
    *outlens = 0;
    cipher_reset(cipher);
    //AES 的整分组成批处理；DES 的分组与末尾不足一组的部分照旧逐组经 cipher_block 校验与填充
    size_t size = 0;
    if (AES_BLOCK_SIZE == cipher->block_lens && lens >= AES_BLOCK_SIZE) {
        size = lens - lens % AES_BLOCK_SIZE;
        _cipher_full_blocks(cipher, data, size, (uint8_t *)output);
    }
    void *buf;
    size_t enlens;
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
            //size < block_lens（含 size==0）属解密失败，避免取末尾分组时下溢越界
            if (size < cipher->block_lens) {
                goto fail;
            }
            uint8_t pad;
            if (ERR_OK != _padding_strip(cipher->padding, (const uint8_t *)output + size - cipher->block_lens,
                                         cipher->block_lens, &pad)) {
                goto fail;
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
