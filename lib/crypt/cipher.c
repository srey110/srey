#include "crypt/cipher.h"
#include "crypt/padding.h"

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
    switch (cipher->model) {
    case CFB:
    case OFB: {
        char *eniv = cipher->_cipher(cipher->cur_ctx, pdiv);
        memcpy(cipher->iv, eniv, cipher->block_lens);
        break;
    }
    case CBC:
    case CTR:
        memcpy(cipher->iv, pdiv, cipher->block_lens);
        break;
    default:
        break;
    }
    cipher_reset(cipher);
}
void cipher_reset(cipher_ctx *cipher) {
    if (ECB != cipher->model) {
        memcpy(cipher->cur_iv, cipher->iv, cipher->block_lens);
    }
}
static const void *_process_data(cipher_ctx *cipher, const void *data, size_t lens, size_t *size) {
    if (lens > cipher->block_lens) {
        return NULL;
    }
    //½âÃÜ
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
    //¼ÓÃÜ ÎÞÌî³ä
    if (NoPadding == cipher->padding) {
        if (lens != cipher->block_lens
            && (ECB == cipher->model || CBC == cipher->model)) {
            return NULL;
        }
        *size = lens;
        return data;
    }
    //Ìî³ä
    if (lens < cipher->block_lens) {
        _padding_data(cipher->padding, data, lens, cipher->pd_data, cipher->block_lens);
        *size = cipher->block_lens;
        return (const void *)cipher->pd_data;
    }
    *size = lens;
    return data;
}
static void _xor_data(cipher_ctx *cipher, const uint8_t *data, const uint8_t *xorbuf, size_t lens) {
    for (size_t i = 0; i < lens; i++) {
        cipher->xor_data[i] = data[i] ^ xorbuf[i];
    }
}
static void _inc_iv(uint8_t *iv, int32_t block_lens, int32_t counter_size) {
    int32_t nonce_idx = block_lens - counter_size;
    for (int32_t idx = block_lens - 1; idx >= nonce_idx; idx--) {
        iv[idx]++;
        if ( 0 != iv[idx]
            || idx == nonce_idx) {
            break;
        }
    }
}
static inline void *_ecb_model(cipher_ctx *cipher, const void *data) {
    return (void *)cipher->_cipher(cipher->cur_ctx, data);
}
static inline void *_cbc_model(cipher_ctx *cipher, const void *data) {
    if (cipher->encrypt) {
        _xor_data(cipher, data, cipher->cur_iv, cipher->block_lens);
        void *en = (void *)cipher->_cipher(cipher->cur_ctx, cipher->xor_data);
        memcpy(cipher->cur_iv, en, cipher->block_lens);
        return en;
    }
    void *de = (void *)cipher->_cipher(cipher->cur_ctx, data);
    _xor_data(cipher, de, cipher->cur_iv, cipher->block_lens);
    memcpy(cipher->cur_iv, data, cipher->block_lens);
    return (void *)cipher->xor_data;
}
static inline void *_cfb_model(cipher_ctx *cipher, const void *data, size_t lens) {
    _xor_data(cipher, data, cipher->cur_iv, lens);
    if (lens == cipher->block_lens) {
        void *en;
        if (cipher->encrypt) {
            en = (void *)cipher->_cipher(cipher->cur_ctx, cipher->xor_data);
        } else {
            en = (void *)cipher->_cipher(cipher->cur_ctx, data);
        }
        memcpy(cipher->cur_iv, en, cipher->block_lens);
    }
    return (void *)cipher->xor_data;
}
static inline void *_ofb_model(cipher_ctx *cipher, const void *data, size_t lens) {
    _xor_data(cipher, data, cipher->cur_iv, lens);
    if (lens == cipher->block_lens) {
        void *en = (void *)cipher->_cipher(cipher->cur_ctx, cipher->cur_iv);
        memcpy(cipher->cur_iv, en, cipher->block_lens);
    }
    return (void *)cipher->xor_data;
}
static inline void *_ctr_model(cipher_ctx *cipher, const void *data, size_t lens) {
    void *en = (void *)cipher->_cipher(cipher->cur_ctx, cipher->cur_iv);
    _xor_data(cipher, data, en, lens);
    if (lens == cipher->block_lens) {
        _inc_iv(cipher->cur_iv, (int32_t)cipher->block_lens, (int32_t)(cipher->block_lens));
    }
    return (void *)cipher->xor_data;
}
void *cipher_block(cipher_ctx *cipher, const void *data, size_t lens, size_t *size) {
    const void *input = _process_data(cipher, data, lens, &lens);
    if (NULL == input) {
        return NULL;
    }
    if (NULL != size) {
        *size = lens;
    }
    void *rtn = NULL;
    switch (cipher->model) {
    case ECB:
        rtn = _ecb_model(cipher, input);
        break;
    case CBC:
        rtn = _cbc_model(cipher, input);
        break;
    case CFB:
        rtn = _cfb_model(cipher, input, lens);
        break;
    case OFB:
        rtn = _ofb_model(cipher, input, lens);
        break;
    case CTR:
        rtn = _ctr_model(cipher, input, lens);
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
                memcpy(output + size, buf, enlens);
                size += enlens;
            }
        } else {
            size -= (uint8_t)output[size - 1];
        }
    }
    return size;
}
