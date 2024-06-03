#include "proto/mysql.h"
#include "proto/mysql_macro.h"
#include "proto/mysql_parse.h"
#include "proto/protos.h"
#include "algo/sha1.h"
#include "algo/sha256.h"
#include "srey/trigger.h"
#include "binary.h"
#if WITH_SSL
#include <openssl/evp.h>
#include <openssl/rsa.h>
#endif

#define MYSQL_AUTH_DEBUG        0
#define MYSQL_HEAD_LENS         4
#define MYSQL_SALT1_LENS        8
#define MYSQL_CACHING_SHA2      "caching_sha2_password"
#define MYSQL_NATIVE_PASSWORLD  "mysql_native_password"
//Capabilities Flags
#define CLIENT_LONG_PASSWORD                  1 //旧密码插件
#define CLIENT_FOUND_ROWS                     2 //Send found rows instead of affected rows in EOF_Packet
#define CLIENT_LONG_FLAG                      4 //Get all column flags
#define CLIENT_CONNECT_WITH_DB                8 //是否带有 dbname
#define CLIENT_ODBC                           64 //odbc
#define CLIENT_LOCAL_FILES                    128 //能否使用 LOAD DATA LOCAL
#define CLIENT_IGNORE_SPACE                   256 //是否忽略 括号( 前面的空格
#define CLIENT_PROTOCOL_41                    512 //New 4.1 protocol. 
#define CLIENT_INTERACTIVE                    1024 //是否为交互式终端
#define CLIENT_SSL                            2048 //是否支持SSL
#define CLIENT_RESERVED2                      32768 //DEPRECATED: Old flag for 4.1 authentication \ CLIENT_SECURE_CONNECTION
#define CLIENT_IGNORE_SIGPIPE                 4096 //网络故障的时候发SIGPIPE
#define CLIENT_TRANSACTIONS                   8192 //OK/EOF包的status_flags
#define CLIENT_MULTI_STATEMENTS               (1UL << 16) //是否支持multi-stmt.  COM_QUERY/COM_STMT_PREPARE中多条语句
#define CLIENT_MULTI_RESULTS                  (1UL << 17) //multi-results
#define CLIENT_PLUGIN_AUTH                    (1UL << 19) //是否支持密码插件 
#define CLIENT_CONNECT_ATTRS                  (1UL << 20) //lient supports connection attributes
#define CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA (1UL << 21) //密码认证包能否大于255字节
#define CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS   (1UL << 22) //不关闭密码过期的连接
#define CLIENT_SESSION_TRACK                  (1UL << 23) //能够处理服务器状态变更信息
#define CLIENT_OPTIONAL_RESULTSET_METADATA    (1UL << 25) //The client can handle optional metadata information in the resultset
#define CLIENT_QUERY_ATTRIBUTES               (1UL << 27) //支持COM_QUERY/COM_STMT_EXECUTE中的可选参数
#define CLIENT_SSL_VERIFY_SERVER_CERT         (1UL << 30) //Verify server certificate
#define CLIENT_CAPS\
    (CLIENT_LONG_PASSWORD | CLIENT_FOUND_ROWS | CLIENT_LONG_FLAG | CLIENT_LOCAL_FILES | CLIENT_IGNORE_SPACE | CLIENT_PROTOCOL_41 |\
    CLIENT_INTERACTIVE | CLIENT_RESERVED2 | CLIENT_IGNORE_SIGPIPE | CLIENT_TRANSACTIONS | CLIENT_MULTI_STATEMENTS |\
    CLIENT_MULTI_RESULTS | CLIENT_PLUGIN_AUTH | CLIENT_CONNECT_ATTRS | CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA |\
    CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS | CLIENT_OPTIONAL_RESULTSET_METADATA | CLIENT_QUERY_ATTRIBUTES)

typedef enum parse_status {
    INIT = 0,
    SSL_EXCHANGE,
    AUTH_PROCESS,
    COMMAND
}parse_status;
typedef struct connect_attr {
    const char *key;
    const char *val;
}connect_attr;
static _handshaked_push _hs_push;

void _mysql_pkfree(void *pack) {
    if (NULL == pack) {
        return;
    }
    mysql_pack_ctx *mpk = pack;
    if (NULL != mpk->_free_mpack) {
        mpk->_free_mpack(mpk->mpack);
    }
    FREE(mpk->mpack);
    FREE(mpk->payload);
    FREE(mpk);
}
void _mysql_udfree(ud_cxt *ud) {
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
void _mysql_init(void *hspush) {
    _hs_push = (_handshaked_push)hspush;
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
static int32_t _mysql_head(buffer_ctx *buf, mysql_params *params, size_t *payload_lens) {
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
static char *_mysql_payload(buffer_ctx *buf, mysql_params *params, size_t *payload_lens, int32_t *status) {
    if (ERR_OK != _mysql_head(buf, params, payload_lens)) {
        BIT_SET(*status, PROTO_MOREDATA);
        return NULL;
    }
    char *payload;
    MALLOC(payload, *payload_lens);
    ASSERTAB(*payload_lens == buffer_remove(buf, payload, *payload_lens), "copy buffer failed.");
    return payload;
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
static void _mysql_connect_attrs(binary_ctx *battrs) {
    connect_attr attrs[] = {
        {"_os", OS_NAME},
        {"_client", "srey"},
    };
    size_t lens;
    for (size_t i = 0; i < ARRAY_SIZE(attrs); i++) {
        lens = strlen(attrs[i].key);
        _mysql_set_fixed_lens_integer(battrs, lens);
        binary_set_string(battrs, attrs[i].key, lens);
        lens = strlen(attrs[i].val);
        _mysql_set_fixed_lens_integer(battrs, lens);
        binary_set_string(battrs, attrs[i].val, lens);
    }
}
static inline void _set_payload_lens(binary_ctx *bwriter) {
    size_t offset = bwriter->offset;
    binary_offset(bwriter, 0);
    binary_set_integer(bwriter, offset - MYSQL_HEAD_LENS, 3, 1);
    binary_offset(bwriter, offset);
}
static void _mysql_auth_response(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud, mysql_params *params) {
    params->id++;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_skip(&bwriter, 3);
    binary_set_int8(&bwriter, params->id);
    binary_set_integer(&bwriter, params->client_caps, 4, 1);//client_flag
    binary_set_integer(&bwriter, params->maxpack, 4, 1);//max_packet_size
    binary_set_uint8(&bwriter, (uint8_t)_mysql_charset(params->charset));//character_set
    binary_set_fill(&bwriter, 0, 23);//filler
    binary_set_string(&bwriter, params->user, strlen(params->user) + 1);//username
    if (0 == strcmp(MYSQL_CACHING_SHA2, params->authv10.auth_plugin)) {
        uint8_t sign[SHA256_BLOCK_SIZE];
        _mysql_caching_sha2_sign(params, sign);
        if (BIT_CHECK(params->client_caps, CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA)) {
            _mysql_set_fixed_lens_integer(&bwriter, sizeof(sign));
            binary_set_string(&bwriter, (const char *)sign, sizeof(sign));//auth_response
        } else {
            binary_set_uint8(&bwriter, (uint8_t)sizeof(sign));
            binary_set_string(&bwriter, (const char *)sign, sizeof(sign));//auth_response
        }
    } else {
        uint8_t sign[SHA1_BLOCK_SIZE];
        _mysql_native_sign(params, sign);
        if (BIT_CHECK(params->client_caps, CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA)){
            _mysql_set_fixed_lens_integer(&bwriter, sizeof(sign));
            binary_set_string(&bwriter, (const char *)sign, sizeof(sign));//auth_response
        } else {
            binary_set_uint8(&bwriter, (uint8_t)sizeof(sign));
            binary_set_string(&bwriter, (const char *)sign, sizeof(sign));//auth_response
        }
    }
    if (BIT_CHECK(params->client_caps, CLIENT_CONNECT_WITH_DB)) {
        binary_set_string(&bwriter, params->database, strlen(params->database) + 1);//database
    }
    if (BIT_CHECK(params->client_caps, CLIENT_PLUGIN_AUTH)){
        binary_set_string(&bwriter, params->authv10.auth_plugin, strlen(params->authv10.auth_plugin) + 1);//client_plugin_name
    }
    if (BIT_CHECK(params->client_caps, CLIENT_CONNECT_ATTRS)) {
        binary_ctx battrs;
        binary_init(&battrs, NULL, 0, 0);
        _mysql_connect_attrs(&battrs);
        _mysql_set_fixed_lens_integer(&bwriter, battrs.offset);
        binary_set_string(&bwriter, battrs.data, battrs.offset);
        FREE(battrs.data);
    }
    _set_payload_lens(&bwriter);
    ev_send(ev, fd, skid, bwriter.data, bwriter.offset, 0);
    ud->status = AUTH_PROCESS;
}
static void _mysql_ssl_exchange(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud, mysql_params *params) {
    params->id++;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_skip(&bwriter, 3);
    binary_set_int8(&bwriter, params->id);
    binary_set_integer(&bwriter, params->client_caps, 4, 1);//client_flag
    binary_set_integer(&bwriter, params->maxpack, 4, 1);//max_packet_size
    binary_set_uint8(&bwriter, (uint8_t)_mysql_charset(params->charset));//character_set
    binary_set_fill(&bwriter, 0, 23);//filler
    _set_payload_lens(&bwriter);
    ev_send(ev, fd, skid, bwriter.data, bwriter.offset, 0);
    ev_ssl(ev, fd, skid, 1, params->evssl);
    ud->status = SSL_EXCHANGE;
}
int32_t _mysql_ssl_exchanged(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud) {
    _mysql_auth_response(ev, fd, skid, ud, ud->extra);
#if MYSQL_AUTH_DEBUG
	LOG_DEBUG("ssl exchange success. auth response:%s.", ((mysql_params *)(ud->extra))->authv10.auth_plugin);
#endif
    return ERR_OK;
}
static void _mysql_auth_request(ev_ctx *ev, SOCKET fd, uint64_t skid,
    buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t payload_lens;
    mysql_params *params = ud->extra;
    char *payload = _mysql_payload(buf, params, &payload_lens, status);
    if (NULL == payload) {
        return;
    }
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
    params->authv10.s1lens = MYSQL_SALT1_LENS;
    params->authv10.salt1 = binary_get_string(&breader, params->authv10.s1lens);//auth-plugin-data-part-1
    binary_get_skip(&breader, 1);//filler
    params->authv10.caps  = binary_get_uint16(&breader, 2, 1);//capability_flags_1
    binary_get_skip(&breader, 3);// character_set 1 status_flags 2
    params->authv10.caps |= ((uint32_t)binary_get_uint16(&breader, 2, 1) << 16);//capability_flags_2
    if (!BIT_CHECK(params->authv10.caps, CLIENT_PROTOCOL_41)
        || !BIT_CHECK(params->authv10.caps, CLIENT_PLUGIN_AUTH)) {
        BIT_SET(*status, PROTO_ERROR);
        FREE(payload);
        LOG_ERROR("CLIENT_PROTOCOL_41 or CLIENT_PLUGIN_AUTH is requred.");
        return;
    }
    params->authv10.payload = payload;
    params->authv10.s2lens = (size_t)binary_get_int8(&breader) - params->authv10.s1lens - 1;//auth_plugin_data_len; 
    binary_get_skip(&breader, 10);//reserved
    params->authv10.salt2 = binary_get_string(&breader, params->authv10.s2lens);//auth-plugin-data-part-2
    binary_get_skip(&breader, 1);
    params->authv10.auth_plugin = binary_get_string(&breader, 0);
    params->client_caps = CLIENT_CAPS;
    if (NULL != params->database
        && 0 != strlen(params->database)) {
        BIT_SET(params->client_caps, CLIENT_CONNECT_WITH_DB);
    }
    if (NULL != ((mysql_params *)ud->extra)->evssl
        && BIT_CHECK(params->authv10.caps, CLIENT_SSL)) {
        BIT_SET(params->client_caps, CLIENT_SSL);
        _mysql_ssl_exchange(ev, fd, skid, ud, params);
#if MYSQL_AUTH_DEBUG
		LOG_DEBUG("ssl exchange.");
#endif
    } else {
        _mysql_auth_response(ev, fd, skid, ud, params);
#if MYSQL_AUTH_DEBUG
		LOG_DEBUG("auth response:%s.", params->authv10.auth_plugin);
#endif
    }
}
static void _mysql_public_key(ev_ctx *ev, SOCKET fd, uint64_t skid, mysql_params *params) {
    params->id++;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_integer(&bwriter, 1, 3, 1);
    binary_set_int8(&bwriter, params->id);
    binary_set_uint8(&bwriter, 0x02);
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
    binary_set_skip(&bwriter, 3);
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
    _set_payload_lens(&bwriter);
    ev_send(ev, fd, skid, bwriter.data, bwriter.offset, 0);
    return ERR_OK;
}
static void _mysql_password_send(ev_ctx *ev, SOCKET fd, uint64_t skid, mysql_params *params) {
    params->id++;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_skip(&bwriter, 3);
    binary_set_int8(&bwriter, params->id);
    binary_set_string(&bwriter, params->password, params->plens + 1);
    _set_payload_lens(&bwriter);
    ev_send(ev, fd, skid, bwriter.data, bwriter.offset, 0);
}
static void _mysql_auth_switch(ev_ctx *ev, SOCKET fd, uint64_t skid, mysql_params *params, mpack_auth_switch *auswitch) {
    params->id++;
    memcpy(params->authv10.salt1, auswitch->provided.data, params->authv10.s1lens);
    memcpy(params->authv10.salt2, (char *)auswitch->provided.data + params->authv10.s1lens, params->authv10.s2lens);
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_skip(&bwriter, 3);
    binary_set_int8(&bwriter, params->id);
    if (0 == strcmp(MYSQL_CACHING_SHA2, auswitch->plugin)) {
        uint8_t sign[SHA256_BLOCK_SIZE];
        _mysql_caching_sha2_sign(params, sign);
        binary_set_string(&bwriter, (const char *)sign, sizeof(sign));
    } else {
        uint8_t sign[SHA1_BLOCK_SIZE];
        _mysql_native_sign(params, sign);
        binary_set_string(&bwriter, (const char *)sign, sizeof(sign));
    }
    _set_payload_lens(&bwriter);
    ev_send(ev, fd, skid, bwriter.data, bwriter.offset, 0);
}
static void _mysql_auth_process(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t payload_lens;
    mysql_params *params = ud->extra;
    char *payload = _mysql_payload(buf, params, &payload_lens, status);
    if (NULL == payload) {
        return;
    }
    binary_ctx breader;
    binary_init(&breader, payload, payload_lens, 0);
    switch (binary_get_uint8(&breader)) {
    case 0x01:
        if (2 == payload_lens) {
            switch (binary_get_uint8(&breader)) {
            case 0x03://fast auth
#if MYSQL_AUTH_DEBUG
				LOG_DEBUG("fast auth.");
#endif
                break;
            case 0x04://full auth
                if (BIT_CHECK(params->client_caps, CLIENT_SSL)) {
                    _mysql_password_send(ev, fd, skid, params);
#if MYSQL_AUTH_DEBUG
					LOG_DEBUG("full auth with ssl, send password.");
#endif
                } else {
                    _mysql_public_key(ev, fd, skid, params);
#if MYSQL_AUTH_DEBUG
					LOG_DEBUG("full auth request public key.");
#endif
                }
                break;
            default:
                BIT_SET(*status, PROTO_ERROR);
                LOG_ERROR("unknow command.");
                break;
            }
        } else if(payload_lens > 2) {
            size_t klens = payload_lens - 1;
#if MYSQL_AUTH_DEBUG
			LOG_DEBUG("full auth got public key, lens:%d.", (int32_t)klens);
#endif
            char *pubkey = binary_get_string(&breader, klens);
            if (ERR_OK != _mysql_full_auth(ev, fd, skid, params, pubkey, klens)){
                BIT_SET(*status, PROTO_ERROR);
                LOG_ERROR("_mysql_full_auth error.");
            }
        } else {
            BIT_SET(*status, PROTO_ERROR);
            LOG_ERROR("unknow command.");
        }
        break;
    case MYSQL_OK:
#if MYSQL_AUTH_DEBUG
		LOG_DEBUG("auth success.");
#endif
        if (ERR_OK != _hs_push(fd, skid, 1, ud, ERR_OK)) {
            BIT_SET(*status, PROTO_ERROR);
        } else {
            ud->status = COMMAND;
        }
        break;
    case MYSQL_EOF: {
        if (payload_lens > 7
            && BIT_CHECK(params->client_caps, CLIENT_PLUGIN_AUTH)) {
            mpack_auth_switch auswitch;
            _mpack_auth_switch(params, &breader, &auswitch);
            _mysql_auth_switch(ev, fd, skid, params, &auswitch);
#if MYSQL_AUTH_DEBUG
			LOG_DEBUG("auth switch:%s.", auswitch.plugin);
#endif
        } else {
            BIT_SET(*status, PROTO_ERROR);
            LOG_ERROR("mysql auth failed.");
        }
        break;
    }
    case MYSQL_ERR: {
        mpack_err err;
        _mpack_err(params, &breader, &err);
        BIT_SET(*status, PROTO_ERROR);
        char *errstr;
        MALLOC(errstr, err.error_msg.lens + 1);
        memcpy(errstr, err.error_msg.data, err.error_msg.lens);
        errstr[err.error_msg.lens] = '\0';
        LOG_ERROR("mysql auth error. %d %s.", err.error_code, errstr);
        FREE(errstr);
        break;
    }
    default:
        BIT_SET(*status, PROTO_ERROR);
        LOG_ERROR("unknow command.");
        break;
    }
    FREE(payload);
}
static mysql_pack_ctx *_mysql_command_process(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t payload_lens;
    mysql_params *params = ud->extra;
    char *payload = _mysql_payload(buf, params, &payload_lens, status);
    if (NULL == payload) {
        return NULL;
    }
    binary_ctx breader;
    binary_init(&breader, payload, payload_lens, 0);
    mysql_pack_ctx *mpack;
    MALLOC(mpack, sizeof(mysql_pack_ctx));
    mpack->sequence_id = params->id;
    mpack->payload_lens = payload_lens;
    mpack->payload = payload;
    mpack->mpack = NULL;
    mpack->_free_mpack = NULL;
    mpack->command = binary_get_uint8(&breader);
    if (ERR_OK != _mpack_parser(params, &breader, mpack)) {
        BIT_SET(*status, PROTO_ERROR);
        _mysql_pkfree(mpack);
        return NULL;
    }
    return mpack;
}
void *mysql_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    mysql_pack_ctx *pack = NULL;
    switch (ud->status) {
    case INIT:
        _mysql_auth_request(ev, fd, skid, buf, ud, status);
        break;
    case AUTH_PROCESS:
        _mysql_auth_process(ev, fd, skid, buf, ud, status);
        break;
    case COMMAND:
        pack = _mysql_command_process(ev, fd, skid, buf, ud, status);
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
void *mysql_pack_quit(size_t *size) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_integer(&bwriter, 1, 3, 1);
    binary_set_int8(&bwriter, 0);
    binary_set_uint8(&bwriter, MYSQL_QUIT);
    *size = bwriter.offset;
    return bwriter.data;
}
void *mysql_pack_selectdb(const char *database, size_t *size) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_skip(&bwriter, 3);
    binary_set_int8(&bwriter, 0);
    binary_set_uint8(&bwriter, MYSQL_INIT_DB);
    binary_set_string(&bwriter, database, strlen(database));
    _set_payload_lens(&bwriter);
    *size = bwriter.offset;
    return bwriter.data;
}
void *mysql_pack_ping(size_t *size) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_integer(&bwriter, 1, 3, 1);
    binary_set_int8(&bwriter, 0);
    binary_set_uint8(&bwriter, MYSQL_PING);
    *size = bwriter.offset;
    return bwriter.data;
}
void *mysql_pack_query(const char *sql, size_t *size) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_skip(&bwriter, 3);
    binary_set_int8(&bwriter, 0);
    binary_set_uint8(&bwriter, MYSQL_QUERY);
    binary_set_string(&bwriter, sql, strlen(sql));
    _set_payload_lens(&bwriter);
    *size = bwriter.offset;
    return bwriter.data;
}
