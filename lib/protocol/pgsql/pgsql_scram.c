#include "protocol/pgsql/pgsql_scram.h"

scram_ctx *scram_init(digest_type dtype) {
    scram_ctx *scram;
    CALLOC(scram, 1, sizeof(scram_ctx));
    scram->dtype = dtype;
    return scram;
}
void scram_free(scram_ctx *scram) {
    if (NULL == scram) {
        return;
    }
    FREE(scram->client_first_message_bare);
    FREE(scram->server_first_message);
    FREE(scram->client_final_message_without_proof);
    FREE(scram->salt);
    FREE(scram->nonce);
    FREE(scram->server_sign);
    FREE(scram);
}
char *scram_client_first_message(scram_ctx *scram, const char *user) {
    char nonce[SCRAM_NONCE_LEN + 1];
    randstr(nonce, SCRAM_NONCE_LEN);
    size_t b64lens = bs64_encode(nonce, SCRAM_NONCE_LEN, scram->client_nonce);
    char *cfbuf;
    if (EMPTYSTR(user)){
        cfbuf = format_va("n,,n=,r=%s", scram->client_nonce);
    } else {
        cfbuf = format_va("n,,n=%s,r=%s", user, scram->client_nonce);
    }
    size_t cfmblens = strlen(cfbuf) - 3;
    CALLOC(scram->client_first_message_bare, 1, cfmblens + 1);
    memcpy(scram->client_first_message_bare, cfbuf + 3, cfmblens);
    return cfbuf;
}
static char *_scram_attr_value(binary_ctx *breader, const char *attr, size_t *lens) {
    size_t off;
    size_t wlens = strlen(attr);
    size_t blens = breader->size - breader->offset;
    char *buf = breader->data + breader->offset;
    char *pos = memstr(0, buf, blens, attr, wlens);
    if (NULL == pos) {
        return NULL;
    }
    char *val = pos + wlens;
    off = val - buf;
    pos = memstr(0, val, blens - off, ",", 1);
    if (NULL == pos) {
        *lens = blens - off;
    } else {
        *lens = pos - val;
    }
    return val;
}
int32_t scram_read_server_first_message(scram_ctx *scram, binary_ctx *breader) {
    size_t lens;
    char *nonce = _scram_attr_value(breader, "r=", &lens);
    if (NULL == nonce) {
        return ERR_FAILED;
    }
    if (lens < strlen(scram->client_nonce)
        || 0 != memcmp(nonce, scram->client_nonce, strlen(scram->client_nonce))) {
        return ERR_FAILED;
    }
    CALLOC(scram->nonce, 1, lens + 1);
    memcpy(scram->nonce, nonce, lens);
    char *salt = _scram_attr_value(breader, "s=", &lens);
    if (NULL == salt) {
        return ERR_FAILED;
    }
    scram->saltlen = (int32_t)B64DE_SIZE(lens);
    MALLOC(scram->salt, scram->saltlen);
    scram->saltlen = (int32_t)bs64_decode(salt, lens, scram->salt);
    char *iter = _scram_attr_value(breader, "i=", &lens);
    if (NULL == iter) {
        return ERR_FAILED;
    }
    char ibuf[8] = { 0 };
    if (lens >= sizeof(ibuf)) {
        return ERR_FAILED;
    }
    memcpy(ibuf, iter, lens);
    scram->iter = atoi(ibuf);
    size_t sfmlens = breader->size - breader->offset;
    CALLOC(scram->server_first_message, 1, sfmlens + 1);
    memcpy(scram->server_first_message, breader->data + breader->offset, sfmlens);
    return ERR_OK;
}
static void _scram_salt_password(scram_ctx *scram, const char *password) {
    char hash[DG_BLOCK_SIZE];
    hmac_ctx hmac;
    hmac_init(&hmac, scram->dtype, password, strlen(password));
    hmac_update(&hmac, scram->salt, scram->saltlen);
    uint32_t one = (uint32_t)htonl(1);
    hmac_update(&hmac, &one, sizeof(one));
    hmac_final(&hmac, hash);
    scram->hslens = (int32_t)hmac_size(&hmac);
    memcpy(scram->saltedpwd, hash, scram->hslens);
    int32_t j;
    for (int32_t i = 1; i < scram->iter; i++) {
        hmac_reset(&hmac);
        hmac_update(&hmac, hash, scram->hslens);
        hmac_final(&hmac, hash);
        for (j = 0; j < scram->hslens; j++) {
            scram->saltedpwd[j] ^= hash[j];
        }
    }
}
static void _scram_key(scram_ctx *scram, const char *key, char result[DG_BLOCK_SIZE]) {
    hmac_ctx hmac;
    hmac_init(&hmac, scram->dtype, scram->saltedpwd, scram->hslens);
    hmac_update(&hmac, key, strlen(key));
    hmac_final(&hmac, result);
}
static void _scram_h(scram_ctx *scram, char client_key[DG_BLOCK_SIZE], char result[DG_BLOCK_SIZE]) {
    digest_ctx digest;
    digest_init(&digest, scram->dtype);
    digest_update(&digest, client_key, scram->hslens);
    digest_final(&digest, result);
}
static void _scram_proof(scram_ctx *scram, char key[DG_BLOCK_SIZE], char result[DG_BLOCK_SIZE]) {
    hmac_ctx hmac;
    hmac_init(&hmac, scram->dtype, key, scram->hslens);
    hmac_update(&hmac, scram->client_first_message_bare, strlen(scram->client_first_message_bare));
    hmac_update(&hmac, ",", 1);
    hmac_update(&hmac, scram->server_first_message, strlen(scram->server_first_message));
    hmac_update(&hmac, ",", 1);
    hmac_update(&hmac, scram->client_final_message_without_proof, strlen(scram->client_final_message_without_proof));
    hmac_final(&hmac, result);
}
char *scram_client_final_message(scram_ctx *scram, const char *pwd) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_va(&bwriter, "c=biws,r=%s", scram->nonce);
    MALLOC(scram->client_final_message_without_proof, bwriter.offset + 1);
    memcpy(scram->client_final_message_without_proof, bwriter.data, bwriter.offset);
    scram->client_final_message_without_proof[bwriter.offset] = '\0';
    _scram_salt_password(scram, pwd);
    char clientkey[DG_BLOCK_SIZE];
    _scram_key(scram, "Client Key", clientkey);
    char storedkey[DG_BLOCK_SIZE];
    _scram_h(scram, clientkey, storedkey);
    char clientsign[DG_BLOCK_SIZE];
    _scram_proof(scram, storedkey, clientsign);
    char client_proof[DG_BLOCK_SIZE];
    for (int32_t i = 0; i < scram->hslens; i++) {
        client_proof[i] = clientkey[i] ^ clientsign[i];
    }
    char b64[B64EN_SIZE(DG_BLOCK_SIZE)];
    bs64_encode(client_proof, scram->hslens, b64);
    binary_set_va(&bwriter, ",p=%s", b64);
    binary_set_int8(&bwriter, 0);
    return bwriter.data;
}
int32_t scram_server_final(scram_ctx *scram, binary_ctx *breader) {
    size_t lens;
    char *error = _scram_attr_value(breader, "e=", &lens);
    if (NULL != error) {
        return ERR_FAILED;
    }
    char *server_sign = _scram_attr_value(breader, "v=", &lens);
    if (NULL == server_sign) {
        return ERR_FAILED;
    }
    MALLOC(scram->server_sign, B64DE_SIZE(lens));
    scram->svsignlens = (int32_t)bs64_decode(server_sign, lens, scram->server_sign);
    char serverkey[DG_BLOCK_SIZE];
    _scram_key(scram, "Server Key", serverkey);
    char hash[DG_BLOCK_SIZE];
    _scram_proof(scram, serverkey, hash);
    if (scram->svsignlens == scram->hslens
        && 0 == memcmp(hash, scram->server_sign, scram->hslens)) {
        return ERR_OK;
    }
    return ERR_FAILED;
}
