#include "proto/mysql.h"
#include "proto/mysql_macro.h"
#include "proto/mysql_parse.h"
#include "proto/protos.h"
#include "algo/sha1.h"
#include "algo/sha256.h"
#include "binary.h"
#include "srey/trigger.h"
#if WITH_SSL
#include <openssl/evp.h>
#include <openssl/rsa.h>
#endif

typedef enum parse_status {
    INIT = 0,
    SSL_EXCHANGE,
    AUTH_RESULT,
    COMMAND
}parse_status;
static _handshaked_push _hs_push;

void mysql_pkfree(void *pack) {
    if (NULL == pack) {
        return;
    }
    mysql_pack_ctx *mpk = pack;
    FREE(mpk->mpack);
    FREE(mpk->payload);
    FREE(mpk);
}
void mysql_udfree(ud_cxt *ud) {
    if (NULL == ud->extra) {
        return;
    }
    mysql_params *params = ud->extra;
    FREE(params->user);
    FREE(params->password);
    FREE(params->database);
    FREE(params->charset);
    FREE(params->authv10.payload);
    FREE(ud->extra);
}
static int32_t _mysql_charset(const char *charset) {
    if(0 == strcmp("big5", charset)) {
        return 1;
    } else if (0 == strcmp("dec8", charset)) {
        return 3;
    } else if (0 == strcmp("cp850", charset)) {
        return 4;
    } else if (0 == strcmp("hp8", charset)) {
        return 6;
    } else if (0 == strcmp("koi8r", charset)) {
        return 7;
    } else if (0 == strcmp("latin1", charset)) {
        return 8;
    } else if (0 == strcmp("latin2", charset)) {
        return 9;
    } else if (0 == strcmp("swe7", charset)) {
        return 10;
    } else if (0 == strcmp("ascii", charset)) {
        return 11;
    } else if (0 == strcmp("ujis", charset)) {
        return 12;
    } else if (0 == strcmp("sjis", charset)) {
        return 13;
    } else if (0 == strcmp("hebrew", charset)) {
        return 16;
    } else if (0 == strcmp("tis620", charset)) {
        return 18;
    } else if (0 == strcmp("euckr", charset)) {
        return 19;
    } else if (0 == strcmp("koi8u", charset)) {
        return 22;
    } else if (0 == strcmp("gb2312", charset)) {
        return 24;
    } else if (0 == strcmp("greek", charset)) {
        return 25;
    } else if (0 == strcmp("cp1250", charset)) {
        return 26;
    } else if (0 == strcmp("gbk", charset)) {
        return 28;
    } else if (0 == strcmp("latin5", charset)) {
        return 30;
    } else if (0 == strcmp("armscii8", charset)) {
        return 32;
    } else if (0 == strcmp("utf8", charset)) {
        return 33;
    } else if (0 == strcmp("cp866", charset)) {
        return 36;
    } else if (0 == strcmp("keybcs2", charset)) {
        return 37;
    } else if (0 == strcmp("macce", charset)) {
        return 38;
    } else if (0 == strcmp("macroman", charset)) {
        return 39;
    } else if (0 == strcmp("cp852", charset)) {
        return 40;
    } else if (0 == strcmp("latin7", charset)) {
        return 41;
    } else if (0 == strcmp("utf8mb4", charset)) {
        return 45;
    } else if (0 == strcmp("cp1251", charset)) {
        return 51;
    } else if (0 == strcmp("utf16le", charset)) {
        return 56;
    } else if (0 == strcmp("cp1256", charset)) {
        return 57;
    } else if (0 == strcmp("cp1257", charset)) {
        return 59;
    } else if (0 == strcmp("binary", charset)) {
        return 63;
    } else if (0 == strcmp("geostd8", charset)) {
        return 92;
    } else if (0 == strcmp("cp932", charset)) {
        return 95;
    } else if (0 == strcmp("eucjpms", charset)) {
        return 97;
    } else if (0 == strcmp("gb18030", charset)) {
        return 248;
    } else {
        return 0;
    }
}
static int32_t _mysql_head(buffer_ctx *buf, size_t *payload_lens, mysql_params *params) {
    size_t size = buffer_size(buf);
    if (size < MYSQL_HEAD_LENS) {
        return ERR_FAILED;
    }
    char head[MYSQL_HEAD_LENS];
    ASSERTAB(sizeof(head) == buffer_copyout(buf, 0, head, sizeof(head)), "copy buffer failed.");
    *payload_lens = (size_t)unpack_integer(head, 3, 1, 0);
    if (size < *payload_lens + sizeof(head)) {
        return ERR_FAILED;
    }
    params->id = head[3];
    ASSERTAB(sizeof(head) == buffer_drain(buf, sizeof(head)), "drain buffer failed.");
    return ERR_OK;
}
static void _mysql_native_sign(mysql_params *params, uint8_t sh1[SHA1_BLOCK_SIZE]) {
    uint8_t shpsw[SHA1_BLOCK_SIZE];
    uint8_t shscr[SHA1_BLOCK_SIZE];
    sha1_ctx sha1;
    sha1_init(&sha1);
    sha1_update(&sha1, (uint8_t *)params->password, params->plens);
    sha1_final(&sha1, shpsw);
    sha1_init(&sha1);
    sha1_update(&sha1, shpsw, SHA1_BLOCK_SIZE);
    sha1_final(&sha1, sh1);
    sha1_init(&sha1);
    sha1_update(&sha1, (uint8_t *)params->authv10.salt1, params->authv10.s1lens);
    sha1_update(&sha1, (uint8_t *)params->authv10.salt2, params->authv10.s2lens);
    sha1_update(&sha1, sh1, SHA1_BLOCK_SIZE);
    sha1_final(&sha1, shscr);
    for (size_t i = 0; i < SHA1_BLOCK_SIZE; i++) {
        sh1[i] = shpsw[i] ^ shscr[i];
    }
}
static void _mysql_caching_sha2_sign(mysql_params *params, uint8_t sh2[SHA256_BLOCK_SIZE]) {
    uint8_t shpsw[SHA256_BLOCK_SIZE];
    uint8_t shscr[SHA256_BLOCK_SIZE];
    sha256_ctx sha256;
    sha256_init(&sha256);
    sha256_update(&sha256, (uint8_t *)params->password, params->plens);
    sha256_final(&sha256, shpsw);
    sha256_init(&sha256);
    sha256_update(&sha256, shpsw, SHA256_BLOCK_SIZE);
    sha256_final(&sha256, sh2);
    sha256_init(&sha256);
    sha256_update(&sha256, sh2, SHA256_BLOCK_SIZE);
    sha256_update(&sha256, (uint8_t *)params->authv10.salt1, params->authv10.s1lens);
    sha256_update(&sha256, (uint8_t *)params->authv10.salt2, params->authv10.s2lens);
    sha256_final(&sha256, shscr);
    for (size_t i = 0; i < SHA256_BLOCK_SIZE; i++) {
        sh2[i] = shpsw[i] ^ shscr[i];
    }
}
static void _mysql_auth_response(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud, mysql_params *params) {
    if (NULL != params->database
        && 0 != strlen(params->database)) {
        BIT_SET(params->client_caps, CLIENT_CONNECT_WITH_DB);
    }
    params->id++;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_fill(&bwriter, 0, 3);
    binary_set_int8(&bwriter, params->id);
    binary_set_integer(&bwriter, params->client_caps, 4, 1);//client_flag
    binary_set_integer(&bwriter, params->maxpack, 4, 1);//max_packet_size
    binary_set_int8(&bwriter, (int8_t)_mysql_charset(params->charset));//character_set
    binary_set_fill(&bwriter, 0, 23);//filler
    binary_set_string(&bwriter, params->user, strlen(params->user) + 1);//username
    if (BIT_CHECK(params->client_caps, CLIENT_PLUGIN_AUTH)) {
        uint8_t sign[SHA256_BLOCK_SIZE];
        _mysql_caching_sha2_sign(params, sign);
        binary_set_int8(&bwriter, (int8_t)sizeof(sign));
        binary_set_string(&bwriter, (const char *)sign, sizeof(sign));//auth_response
    } else {
        uint8_t sign[SHA1_BLOCK_SIZE];
        _mysql_native_sign(params, sign);
        binary_set_int8(&bwriter, (int8_t)sizeof(sign));
        binary_set_string(&bwriter, (const char *)sign, sizeof(sign));//auth_response
    }
    if (BIT_CHECK(params->client_caps, CLIENT_CONNECT_WITH_DB)) {
        binary_set_string(&bwriter, params->database, strlen(params->database) + 1);//database
    }
    if (BIT_CHECK(params->client_caps, CLIENT_PLUGIN_AUTH)) {
        binary_set_string(&bwriter, params->authv10.auth_plugin, strlen(params->authv10.auth_plugin) + 1);//client_plugin_name
    }
    size_t lens = bwriter.offset;
    binary_offset(&bwriter, 0);
    binary_set_integer(&bwriter, lens - MYSQL_HEAD_LENS, 3, 1);
    ev_send(ev, fd, skid, bwriter.data, lens, 0);
    ud->status = AUTH_RESULT;
}
static void _mysql_ssl_exchange(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud, mysql_params *params) {
    params->id++;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_fill(&bwriter, 0, 3);
    binary_set_int8(&bwriter, params->id);    
    binary_set_integer(&bwriter, params->client_caps, 4, 1);//client_flag
    binary_set_integer(&bwriter, params->maxpack, 4, 1);//max_packet_size
    binary_set_int8(&bwriter, (int8_t)_mysql_charset(params->charset));//character_set
    binary_set_fill(&bwriter, 0, 23);//filler
    size_t lens = bwriter.offset;
    binary_offset(&bwriter, 0);
    binary_set_integer(&bwriter, lens - MYSQL_HEAD_LENS, 3, 1);
    ev_send(ev, fd, skid, bwriter.data, lens, 0);
    ev_ssl(ev, fd, skid, 1, params->evssl);
    ud->status = SSL_EXCHANGE;
}
int32_t mysql_ssl_exchanged(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud) {
    _mysql_auth_response(ev, fd, skid, ud, ud->extra);
    return ERR_OK;
}
static void _mysql_auth_request(ev_ctx *ev, SOCKET fd, uint64_t skid,
    buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t payload_lens;
    mysql_params *params = ud->extra;
    if (ERR_OK != _mysql_head(buf, &payload_lens, params)) {
        BIT_SET(*status, PROTO_MOREDATA);
        return;
    }
    char *payload;
    MALLOC(payload, payload_lens);
    ASSERTAB(payload_lens == buffer_remove(buf, payload, payload_lens), "copy buffer failed.");
    binary_ctx breader;
    binary_init(&breader, payload, payload_lens, 0);
    if (0x0a != binary_get_int8(&breader)) {//protocol version
        BIT_SET(*status, PROTO_ERROR);
        FREE(payload);
        LOG_ERROR("mysql protocol version not 0x0a.");
        return;
    }
    params->authv10.svver = binary_get_string(&breader, 0);//server version
    binary_get_skip(&breader, 4);//thread id
    params->authv10.s1lens = 8;
    params->authv10.salt1 = binary_get_string(&breader, params->authv10.s1lens);//auth-plugin-data-part-1
    binary_get_skip(&breader, 1);//filler
    params->authv10.caps  = binary_get_uint16(&breader, 2, 1);//capability_flags_1
    binary_get_skip(&breader, 3);// character_set 1 status_flags 2
    params->authv10.caps |= ((uint32_t)binary_get_uint16(&breader, 2, 1) << 16);//capability_flags_2
    if (!BIT_CHECK(params->authv10.caps, CLIENT_PROTOCOL_41)) {
        BIT_SET(*status, PROTO_ERROR);
        FREE(payload);
        LOG_ERROR("mysql protocol not new 4.1 protocol.");
        return;
    }
    params->authv10.payload = payload;
    params->authv10.s2lens = (size_t)binary_get_int8(&breader) - params->authv10.s1lens - 1;//auth_plugin_data_len; 
    binary_get_skip(&breader, 10);//reserved
    params->authv10.salt2 = binary_get_string(&breader, params->authv10.s2lens);//auth-plugin-data-part-2
    binary_get_skip(&breader, 1);
    params->authv10.auth_plugin = binary_get_string(&breader, 0);
    params->client_caps = MYSQL_CLIENT_CAPS;
    if (0 == strcmp(MYSQL_CACHING_SHA2, params->authv10.auth_plugin)) {
        BIT_SET(params->client_caps, CLIENT_PLUGIN_AUTH);
    } else {
        BIT_SET(params->client_caps, CLIENT_RESERVED2);
    }
    if (NULL != ((mysql_params *)ud->extra)->evssl
        && BIT_CHECK(params->authv10.caps, CLIENT_SSL)) {
        BIT_SET(params->client_caps, CLIENT_SSL);
        _mysql_ssl_exchange(ev, fd, skid, ud, params);
    } else {
        _mysql_auth_response(ev, fd, skid, ud, params);
    }
}
static void _mysql_public_key(ev_ctx *ev, SOCKET fd, uint64_t skid, mysql_params *params) {
    params->id++;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_integer(&bwriter, 1, 3, 1);
    binary_set_int8(&bwriter, params->id);
    binary_set_int8(&bwriter, 0x02);
    ev_send(ev, fd, skid, bwriter.data, bwriter.offset, 0);
}
static char *_mysql_password_xor_salt(mysql_params *params, size_t *lens) {
    *lens = params->plens + 1;
    char *xorpsw;
    MALLOC(xorpsw, *lens);
    memcpy(xorpsw, params->password, params->plens);
    xorpsw[params->plens] = '\0';
    size_t index, slens = params->authv10.s1lens + params->authv10.s2lens;
    for (size_t i = 0; i < *lens; i++) {
        index = i % slens;
        if (index < params->authv10.s1lens) {
            xorpsw[i] ^= params->authv10.salt1[index];
        } else {
            xorpsw[i] ^= params->authv10.salt2[index - params->authv10.s1lens];
        }
    }
    return xorpsw;
}
#if WITH_SSL
static EVP_PKEY_CTX *_mysql_encrypt_init(char *pubkey, size_t klens) {
    BIO *bio = BIO_new(BIO_s_mem());
    if (NULL == bio) {
        return NULL;
    }
    BIO_write(bio, pubkey, (int32_t)klens);
    EVP_PKEY *evpkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (NULL == evpkey) {
        return NULL;
    }
    EVP_PKEY_CTX *evpctx = EVP_PKEY_CTX_new(evpkey, NULL);
    EVP_PKEY_free(evpkey);
    if (NULL == evpctx) {
        return NULL;
    }
    if (0 >= EVP_PKEY_encrypt_init(evpctx)) {
        EVP_PKEY_CTX_free(evpctx);
        return NULL;
    }
    if (0 >= EVP_PKEY_CTX_set_rsa_padding(evpctx, RSA_PKCS1_OAEP_PADDING)) {
        EVP_PKEY_CTX_free(evpctx);
        return NULL;
    }
    return evpctx;
}
#endif
static int32_t _mysql_sha2_rsa(binary_ctx *bwriter, char *pubkey, size_t klens, char *xorpsw, size_t xlens) {
#if WITH_SSL
    EVP_PKEY_CTX *evpctx = _mysql_encrypt_init(pubkey, klens);
    if (NULL == evpctx) {
        LOG_DEBUG("_mysql_encrypt_init error.");
        return ERR_FAILED;
    }
    //取得加密后长度
    size_t enlens;
    if (0 >= EVP_PKEY_encrypt(evpctx, NULL, &enlens, NULL, 0)) {
        EVP_PKEY_CTX_free(evpctx);
        LOG_DEBUG("EVP_PKEY_encrypt error.");
        return ERR_FAILED;
    }
    size_t offset, outlens, block_size = enlens  - 42;
    for (size_t i = 0; i < xlens; i += block_size) {
        offset = bwriter->offset;
        binary_set_skip(bwriter, enlens);
        if (0 >= EVP_PKEY_encrypt(evpctx, (unsigned char *)(bwriter->data + offset), &outlens,
            (unsigned char *)(xorpsw + i), ((i + block_size > xlens) ? (xlens - i) : block_size))) {
            EVP_PKEY_CTX_free(evpctx);
            LOG_DEBUG("EVP_PKEY_encrypt error.");
            return ERR_FAILED;
        }
    }
    EVP_PKEY_CTX_free(evpctx);
    return ERR_OK;
#else
    return ERR_FAILED;
#endif
}
static int32_t _mysql_full_auth(ev_ctx *ev, SOCKET fd, uint64_t skid,
    mysql_params *params, char *pubkey, size_t klens) {
    params->id++;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 300, 0);
    binary_set_fill(&bwriter, 0, 3);
    binary_set_int8(&bwriter, params->id);
    size_t lens;
    char *xorpsw = _mysql_password_xor_salt(params, &lens);
    if (ERR_OK != _mysql_sha2_rsa(&bwriter, pubkey, klens, xorpsw, lens)) {
        FREE(xorpsw);
        FREE(bwriter.data);
        LOG_ERROR("_mysql_sha2_rsa error.");
        return ERR_FAILED;
    }
    FREE(xorpsw);
    lens = bwriter.offset;
    binary_offset(&bwriter, 0);
    binary_set_integer(&bwriter, lens - MYSQL_HEAD_LENS, 3, 1);
    ev_send(ev, fd, skid, bwriter.data, lens, 0);
    return ERR_OK;
}
static void _mysql_password_send(ev_ctx *ev, SOCKET fd, uint64_t skid, mysql_params *params) {
    params->id++;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_fill(&bwriter, 0, 3);
    binary_set_int8(&bwriter, params->id);
    binary_set_string(&bwriter, params->password, params->plens + 1);
    size_t size = bwriter.offset;
    binary_offset(&bwriter, 0);
    binary_set_integer(&bwriter, size - MYSQL_HEAD_LENS, 3, 1);
    ev_send(ev, fd, skid, bwriter.data, size, 0);
}
static void _mysql_auth_switch(ev_ctx *ev, SOCKET fd, uint64_t skid, mysql_params *params, mpack_auth_switch *auswitch) {
    params->id++;
    memcpy(params->authv10.salt1, auswitch->provided.data, params->authv10.s1lens);
    memcpy(params->authv10.salt2, (char *)auswitch->provided.data + params->authv10.s1lens, params->authv10.s2lens);
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_fill(&bwriter, 0, 3);
    binary_set_int8(&bwriter, params->id);
    if (0 == strcmp(auswitch->plugin, MYSQL_CACHING_SHA2)) {
        uint8_t sign[SHA256_BLOCK_SIZE];
        _mysql_caching_sha2_sign(params, sign);
        binary_set_string(&bwriter, (const char *)sign, sizeof(sign));
    } else {
        uint8_t sign[SHA1_BLOCK_SIZE];
        _mysql_native_sign(params, sign);
        binary_set_string(&bwriter, (const char *)sign, sizeof(sign));
    }
    size_t size = bwriter.offset;
    binary_offset(&bwriter, 0);
    binary_set_integer(&bwriter, size - MYSQL_HEAD_LENS, 3, 1);
    ev_send(ev, fd, skid, bwriter.data, size, 0);
}
static void _mysql_auth_result(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t payload_lens;
    mysql_params *params = ud->extra;
    if (ERR_OK != _mysql_head(buf, &payload_lens, params)) {
        BIT_SET(*status, PROTO_MOREDATA);
        return;
    }
    char *payload;
    MALLOC(payload, payload_lens);
    ASSERTAB(payload_lens == buffer_remove(buf, payload, payload_lens), "copy buffer failed.");
    binary_ctx breader;
    binary_init(&breader, payload, payload_lens, 0);
    uint8_t cmd = binary_get_uint8(&breader);
    switch (cmd) {
    case 0x01:
        if (2 == payload_lens) {
            switch (binary_get_uint8(&breader)) {
            case 0x03://fast auth wait result
                break;
            case 0x04://full auth
                if (BIT_CHECK(params->client_caps, CLIENT_SSL)) {
                    _mysql_password_send(ev, fd, skid, params);
                } else {
                    _mysql_public_key(ev, fd, skid, params);
                }
                break;
            default:
                BIT_SET(*status, PROTO_ERROR);
                LOG_ERROR("unknow command.");
                break;
            }
        } else if(payload_lens > 2) {
            size_t klens = payload_lens - 1;
            char *pubkey = binary_get_string(&breader, klens);
            if (ERR_OK != _mysql_full_auth(ev, fd, skid, params, pubkey, klens)){
                BIT_SET(*status, PROTO_ERROR);
                LOG_ERROR("_mysql_full_auth error.");
            }
        } else {
            BIT_SET(*status, PROTO_ERROR);
            LOG_ERROR("payload lens error.");
        }
        break;
    case MYSQL_PACK_OK:
        if (ERR_OK != _hs_push(fd, skid, 1, ud, ERR_OK)) {
            BIT_SET(*status, PROTO_ERROR);
        } else {
            ud->status = COMMAND;
        }
        break;
    case MYSQL_PACK_EOF: {
        if (payload_lens > 7
            && BIT_CHECK(params->client_caps, CLIENT_PLUGIN_AUTH)) {
            mpack_auth_switch auswitch;
            _mpack_auth_switch(params, &breader, &auswitch);
            _mysql_auth_switch(ev, fd, skid, params, &auswitch);
        } else {
            BIT_SET(*status, PROTO_ERROR);
            LOG_ERROR("mysql auth failed.");
        }
        break;
    }
    case MYSQL_PACK_ERR: {
        mpack_err err;
        _mpack_err(params, &breader, &err);
        BIT_SET(*status, PROTO_ERROR);
        LOG_ERROR("mysql auth error. %d %s.", err.error_code, err.error_msg);
        break;
    }
    default:
        BIT_SET(*status, PROTO_ERROR);
        LOG_ERROR("unknow command.");
        break;
    }
    FREE(payload);
}
static mysql_pack_ctx *_mysql_command(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t payload_lens;
    mysql_params *params = ud->extra;
    if (ERR_OK != _mysql_head(buf, &payload_lens, params)) {
        BIT_SET(*status, PROTO_MOREDATA);
        return NULL;
    }
    char *payload;
    MALLOC(payload, payload_lens);
    ASSERTAB(payload_lens == buffer_remove(buf, payload, payload_lens), "copy buffer failed.");
    mysql_pack_ctx *pk;
    MALLOC(pk, sizeof(mysql_pack_ctx));
    pk->sequence_id = params->id;
    pk->payload_lens = payload_lens;
    pk->payload = payload;
    pk->mpack = NULL;
    binary_ctx breader;
    binary_init(&breader, payload, payload_lens, 0);
    if (ERR_OK != _mpack_parser(params, &breader, pk)) {
        BIT_SET(*status, PROTO_ERROR);
        mysql_pkfree(pk);
        return NULL;
    }
    return pk;
}
void *mysql_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    mysql_pack_ctx *pack = NULL;
    switch (ud->status) {
    case INIT:
        _mysql_auth_request(ev, fd, skid, buf, ud, status);
        break;
    case AUTH_RESULT:
        _mysql_auth_result(ev, fd, skid, buf, ud, status);
        break;
    case COMMAND:
        pack = _mysql_command(ev, fd, skid, buf, ud, status);
        break;
    default:
        BIT_SET(*status, PROTO_ERROR);
        break;
    }
    return pack;
}
static char *_copy_param(const char *arg) {
    if (NULL == arg) {
        return NULL;
    }
    size_t lens = strlen(arg);
    char *p;
    MALLOC(p, lens + 1);
    memcpy(p, arg, lens);
    p[lens] = '\0';
    return p;
}
SOCKET mysql_connect(task_ctx *task, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, const char *database, const char *charset, int32_t maxpk, uint64_t *skid) {
    mysql_params *params;
    CALLOC(params, 1, sizeof(mysql_params));
    params->maxpack = 0 == maxpk ? ONEK * ONEK : maxpk;
    params->evssl = evssl;
    params->user = _copy_param(user);
    params->password = _copy_param(password);
    if (NULL != params->password) {
        params->plens = strlen(params->password);
    }
    params->database = _copy_param(database);
    params->charset = _copy_param(charset);
    return trigger_conn_extra(task, PACK_MYSQL, params, ip, port, skid, NULL == evssl ? NETEV_NONE : NETEV_AUTHSSL);
}
void _mysql_init(void *hspush) {
    _hs_push = (_handshaked_push)hspush;
}
