#include "crypt/crypt.h"
#include "crypt/padding.h"

void crypt_init(crypt_ctx *crypt, engine_type engine, cipher_model model,
    const char *key, size_t klens, int32_t keybits, int32_t encrypt) {
    crypt->encrypt = encrypt;
    crypt->model = model;
    crypt->padding = NoPadding;
    if (AES == engine) {
        crypt->block_lens = AES_BLOCK_SIZE;
        crypt->cur_ctx = &crypt->eng_ctx.aes;
        crypt->_crypt = (_crypt_cb)aes_crypt;
        if (crypt->encrypt
            || CFB == crypt->model
            || OFB == crypt->model
            || CTR == crypt->model) {
            aes_init(crypt->cur_ctx, key, klens, keybits, 1);
        } else {
            aes_init(crypt->cur_ctx, key, klens, keybits, 0);
        }
        return;
    }
    crypt->block_lens = DES_BLOCK_SIZE;
    crypt->cur_ctx = &crypt->eng_ctx.des;
    crypt->_crypt = (_crypt_cb)des_crypt;
    if (crypt->encrypt
        || CFB == crypt->model
        || OFB == crypt->model
        || CTR == crypt->model) {
        des_init(crypt->cur_ctx, key, klens, DES3 == engine, 1);
    } else {
        des_init(crypt->cur_ctx, key, klens, DES3 == engine, 0);
    }
}
size_t crypt_block_size(crypt_ctx *crypt) {
    return crypt->block_lens;
}
void crypt_padding(crypt_ctx *crypt, padding_model padding) {
    crypt->padding = padding;
}
void crypt_iv(crypt_ctx *crypt, const char *iv, size_t ilens) {
    if (ECB == crypt->model) {
        return;
    }
    uint8_t pdiv[MAX_BLOCK_SIZE];
    uint8_t *kiv = _padding_key(iv, ilens, pdiv, crypt->block_lens);
    switch (crypt->model) {
    case CFB:
    case OFB: {
        char *eniv = crypt->_crypt(crypt->cur_ctx, kiv);
        memcpy(crypt->iv, eniv, crypt->block_lens);
        break;
    }
    case CBC:
    case CTR:
        memcpy(crypt->iv, kiv, crypt->block_lens);
        break;
    default:
        break;
    }
    crypt_clear(crypt);
}
void crypt_clear(crypt_ctx *crypt) {
    if (ECB != crypt->model) {
        memcpy(crypt->cur_iv, crypt->iv, crypt->block_lens);
    }
}
static const void *_process_data(crypt_ctx *crypt, const void *data, size_t lens, size_t *size) {
    if (lens > crypt->block_lens) {
        return NULL;
    }
    //½âÃÜ
    if (!crypt->encrypt) {
        if (lens != crypt->block_lens) {
            if (ECB == crypt->model
                || CBC == crypt->model) {
                return NULL;
            }
            if (NoPadding != crypt->padding) {
                return NULL;
            }
        }
        *size = lens;
        return data;
    }
    //¼ÓÃÜ ÎÞÌî³ä
    if (NoPadding == crypt->padding) {
        if (lens != crypt->block_lens
            && (ECB == crypt->model || CBC == crypt->model)) {
            return NULL;
        }
        *size = lens;
        return data;
    }
    //Ìî³ä
    if (lens < crypt->block_lens) {
        _padding_data(crypt->padding, data, lens, crypt->pd_data, crypt->block_lens);
        *size = crypt->block_lens;
        return (const void *)crypt->pd_data;
    }
    *size = lens;
    return data;
}
static void _xor_data(crypt_ctx *crypt, const uint8_t *data, const uint8_t *xorbuf, size_t lens) {
    for (size_t i = 0; i < lens; i++) {
        crypt->xor_data[i] = data[i] ^ xorbuf[i];
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
static inline void *_ecb_crypt(crypt_ctx *crypt, const void *data) {
    return (void *)crypt->_crypt(crypt->cur_ctx, data);
}
static inline void *_cbc_crypt(crypt_ctx *crypt, const void *data) {
    if (crypt->encrypt) {
        _xor_data(crypt, data, crypt->cur_iv, crypt->block_lens);
        void *en = (void *)crypt->_crypt(crypt->cur_ctx, crypt->xor_data);
        memcpy(crypt->cur_iv, en, crypt->block_lens);
        return en;
    }
    void *de = (void *)crypt->_crypt(crypt->cur_ctx, data);
    _xor_data(crypt, de, crypt->cur_iv, crypt->block_lens);
    memcpy(crypt->cur_iv, data, crypt->block_lens);
    return (void *)crypt->xor_data;
}
static inline void *_cfb_crypt(crypt_ctx *crypt, const void *data, size_t lens) {
    _xor_data(crypt, data, crypt->cur_iv, lens);
    if (lens == crypt->block_lens) {
        void *en;
        if (crypt->encrypt) {
            en = (void *)crypt->_crypt(crypt->cur_ctx, crypt->xor_data);
        } else {
            en = (void *)crypt->_crypt(crypt->cur_ctx, data);
        }
        memcpy(crypt->cur_iv, en, crypt->block_lens);
    }
    return (void *)crypt->xor_data;
}
static inline void *_ofb_crypt(crypt_ctx *crypt, const void *data, size_t lens) {
    _xor_data(crypt, data, crypt->cur_iv, lens);
    if (lens == crypt->block_lens) {
        void *en = (void *)crypt->_crypt(crypt->cur_ctx, crypt->cur_iv);
        memcpy(crypt->cur_iv, en, crypt->block_lens);
    }
    return (void *)crypt->xor_data;
}
static inline void *_ctr_crypt(crypt_ctx *crypt, const void *data, size_t lens) {
    void *en = (void *)crypt->_crypt(crypt->cur_ctx, crypt->cur_iv);
    _xor_data(crypt, data, en, lens);
    if (lens == crypt->block_lens) {
        _inc_iv(crypt->cur_iv, (int32_t)crypt->block_lens, (int32_t)(crypt->block_lens / 2));
    }
    return (void *)crypt->xor_data;
}
void *crypt_block(crypt_ctx *crypt, const void *data, size_t lens, size_t *size) {
    const void *input = _process_data(crypt, data, lens, &lens);
    if (NULL == input) {
        return NULL;
    }
    if (NULL != size) {
        *size = lens;
    }
    void *rtn = NULL;
    switch (crypt->model) {
    case ECB:
        rtn = _ecb_crypt(crypt, input);
        break;
    case CBC:
        rtn = _cbc_crypt(crypt, input);
        break;
    case CFB:
        rtn = _cfb_crypt(crypt, input, lens);
        break;
    case OFB:
        rtn = _ofb_crypt(crypt, input, lens);
        break;
    case CTR:
        rtn = _ctr_crypt(crypt, input, lens);
        break;
    default:
        break;
    }
    return rtn;
}
size_t crypt_dofinal(crypt_ctx *crypt, const void *data, size_t lens, char *output) {
    void *buf;
    size_t enlens, size = 0;
    crypt_clear(crypt);
    for (size_t i = 0; i < lens; i += crypt->block_lens) {
        enlens = (i + crypt->block_lens > lens ? lens - i : crypt->block_lens);
        buf = crypt_block(crypt, (const char *)data + i, enlens, &enlens);
        if (NULL == buf) {
            return size;
        }
        memcpy(output + size, buf, enlens);
        size += enlens;
    }
    if (PKCS57 == crypt->padding
        || ISO10126 == crypt->padding
        || ANSIX923 == crypt->padding) {
        if (crypt->encrypt) {
            if (0 == lens % crypt->block_lens) {
                _padding_data(crypt->padding, NULL, 0, crypt->pd_data, crypt->block_lens);
                buf = crypt_block(crypt, crypt->pd_data, crypt->block_lens, &enlens);
                memcpy(output + size, buf, enlens);
                size += enlens;
            }
        } else {
            size -= (uint8_t)output[size - 1];
        }
    }
    return size;
}
