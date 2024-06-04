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

#define MYSQL_HEAD_LENS              4
#define MYSQL_AUTH_SWITCH            0xfe
#define MYSQL_CACHING_SHA2           0x01
#define MYSQL_CACHING_SHA2_FAST      0x03
#define MYSQL_CACHING_SHA2_FULL      0x04
#define CACHING_SHA2_PASSWORLD       "caching_sha2_password"
#define MYSQL_NATIVE_PASSWORLD       "mysql_native_password"
//Capabilities Flags
#define CLIENT_LONG_PASSWORD                  1 //旧密码插件
#define CLIENT_LONG_FLAG                      4 //Get all column flags
#define CLIENT_CONNECT_WITH_DB                8 //是否带有 dbname
#define CLIENT_ODBC                           64 //Special handling of ODBC behavior
#define CLIENT_LOCAL_FILES                    128 //能否使用 LOAD DATA LOCAL
#define CLIENT_IGNORE_SPACE                   256 //是否忽略 括号( 前面的空格
#define CLIENT_PROTOCOL_41                    512 //New 4.1 protocol. 
#define CLIENT_INTERACTIVE                    1024 //是否为交互式终端
#define CLIENT_SSL                            2048 //是否支持SSL
#define CLIENT_TRANSACTIONS                   8192 //OK/EOF包的status_flags
#define CLIENT_RESERVED2                      32768 //DEPRECATED: Old flag for 4.1 authentication \ CLIENT_SECURE_CONNECTION
#define CLIENT_MULTI_STATEMENTS               (1UL << 16) //是否支持multi-stmt.  COM_QUERY/COM_STMT_PREPARE中多条语句
#define CLIENT_MULTI_RESULTS                  (1UL << 17) //multi-results
#define CLIENT_PS_MULTI_RESULTS               (1UL << 18) //Multi-results and OUT parameters in PS-protocol.
#define CLIENT_PLUGIN_AUTH                    (1UL << 19) //是否支持密码插件 
#define CLIENT_CONNECT_ATTRS                  (1UL << 20) //client supports connection attributes
#define CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA (1UL << 21) //密码认证包能否大于255字节
#define CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS   (1UL << 22) //不关闭密码过期的连接
#define CLIENT_SESSION_TRACK                  (1UL << 23) //能够处理服务器状态变更信息
#define CLIENT_DEPRECATE_EOF                  (1UL << 24) //Client no longer needs EOF_Packet and will use OK_Packet instead
#define CLIENT_OPTIONAL_RESULTSET_METADATA    (1UL << 25) //The client can handle optional metadata information in the resultset. 
#define CLIENT_QUERY_ATTRIBUTES               (1UL << 27) //支持COM_QUERY/COM_STMT_EXECUTE中的可选参数
#define CLIENT_CAPABILITY_EXTENSION           (1UL << 29) //This flag will be reserved to extend the 32bit capabilities structure to 64bits
#define CLIENT_SSL_VERIFY_SERVER_CERT         (1UL << 30) //Verify server certificate
#define CLIENT_REMEMBER_OPTIONS               (1UL << 31) //Don't reset the options after an unsuccessful connect.
#define CLIENT_CAPS\
    (CLIENT_LONG_PASSWORD | CLIENT_LONG_FLAG | CLIENT_LOCAL_FILES | CLIENT_PROTOCOL_41 |\
    CLIENT_INTERACTIVE | CLIENT_TRANSACTIONS | CLIENT_RESERVED2 | CLIENT_MULTI_STATEMENTS |\
    CLIENT_MULTI_RESULTS | CLIENT_PS_MULTI_RESULTS | CLIENT_PLUGIN_AUTH | CLIENT_CONNECT_ATTRS |\
    CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA | CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS | CLIENT_SESSION_TRACK |\
    CLIENT_OPTIONAL_RESULTSET_METADATA)

typedef enum parse_status {
    INIT = 0,
    SSL_EXCHANGE,
    AUTH_PROCESS,
    COMMAND
}parse_status;
typedef enum client_status {
    LINKED = 0x01,
    AUTHED = 0x02
}client_status;
typedef struct mpack_auth_switch {
    char *plugin;//name of the client authentication plugin to switch to
    buf_ctx provided;//Initial authentication data for that client plugin
}mpack_auth_switch;
typedef struct connect_attr {
    const char *key;
    const char *val;
}connect_attr;
static _handshaked_push _hs_push;

void _mysql_pkfree(void *pack) {
    if (NULL == pack) {
        return;
    }
    mpack_ctx *mpack = pack;
    if (NULL != mpack->_free_mpack) {
        mpack->_free_mpack(mpack->mpack);
    }
    FREE(mpack->mpack);
    FREE(mpack->payload);
    FREE(mpack);
}
void _mysql_udfree(ud_cxt *ud) {
    if (NULL == ud->extra) {
        return;
    }
    mysql_ctx *mysql = ud->extra;
    mysql->status = 0;
    mysql->cur_cmd = 0;
    ud->extra = NULL;
}
void _mysql_init(void *hspush) {
    _hs_push = (_handshaked_push)hspush;
}
static uint8_t _mysql_charset(const char *charset) {
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
static int32_t _mysql_head(mysql_ctx *mysql, buffer_ctx *buf, size_t *payload_lens) {
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
    mysql->id = head[3];
    ASSERTAB(sizeof(head) == buffer_drain(buf, sizeof(head)), "drain buffer failed.");
    return ERR_OK;
}
static char *_mysql_payload(mysql_ctx *mysql, buffer_ctx *buf, size_t *payload_lens, int32_t *status) {
    if (ERR_OK != _mysql_head(mysql, buf, payload_lens)) {
        BIT_SET(*status, PROTO_MOREDATA);
        return NULL;
    }
    char *payload;
    MALLOC(payload, *payload_lens);
    ASSERTAB(*payload_lens == buffer_remove(buf, payload, *payload_lens), "copy buffer failed.");
    return payload;
}
static void _mysql_native_sign(mysql_ctx *mysql, uint8_t sh1[SHA1_BLOCK_SIZE]) {
    uint8_t shpsw[SHA1_BLOCK_SIZE];
    uint8_t shscr[SHA1_BLOCK_SIZE];
    sha1_ctx sha1;
    sha1_init(&sha1);
    sha1_update(&sha1, (uint8_t *)mysql->password, strlen(mysql->password));
    sha1_final(&sha1, shpsw);
    sha1_init(&sha1);
    sha1_update(&sha1, shpsw, SHA1_BLOCK_SIZE);
    sha1_final(&sha1, sh1);
    sha1_init(&sha1);
    sha1_update(&sha1, (uint8_t *)mysql->salt, sizeof(mysql->salt));
    sha1_update(&sha1, sh1, SHA1_BLOCK_SIZE);
    sha1_final(&sha1, shscr);
    for (size_t i = 0; i < SHA1_BLOCK_SIZE; i++) {
        sh1[i] = shpsw[i] ^ shscr[i];
    }
}
static void _mysql_caching_sha2_sign(mysql_ctx *mysql, uint8_t sh2[SHA256_BLOCK_SIZE]) {
    uint8_t shpsw[SHA256_BLOCK_SIZE];
    uint8_t shscr[SHA256_BLOCK_SIZE];
    sha256_ctx sha256;
    sha256_init(&sha256);
    sha256_update(&sha256, (uint8_t *)mysql->password, strlen(mysql->password));
    sha256_final(&sha256, shpsw);
    sha256_init(&sha256);
    sha256_update(&sha256, shpsw, SHA256_BLOCK_SIZE);
    sha256_final(&sha256, sh2);
    sha256_init(&sha256);
    sha256_update(&sha256, sh2, SHA256_BLOCK_SIZE);
    sha256_update(&sha256, (uint8_t *)mysql->salt, sizeof(mysql->salt));
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
        _mysql_set_lenenc_int(battrs, lens);
        binary_set_string(battrs, attrs[i].key, lens);
        lens = strlen(attrs[i].val);
        _mysql_set_lenenc_int(battrs, lens);
        binary_set_string(battrs, attrs[i].val, lens);
    }
}
static inline void _set_payload_lens(binary_ctx *bwriter) {
    size_t size = bwriter->offset;
    binary_offset(bwriter, 0);
    binary_set_integer(bwriter, size - MYSQL_HEAD_LENS, 3, 1);
    binary_offset(bwriter, size);
}
static void _mysql_auth_response(mysql_ctx *mysql, ev_ctx *ev, ud_cxt *ud) {
    mysql->id++;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_skip(&bwriter, 3);
    binary_set_int8(&bwriter, mysql->id);
    binary_set_integer(&bwriter, mysql->client_caps, 4, 1);//client_flag
    binary_set_integer(&bwriter, mysql->maxpack, 4, 1);//max_packet_size
    binary_set_uint8(&bwriter, mysql->client_charset);//character_set
    binary_set_fill(&bwriter, 0, 23);//filler
    binary_set_string(&bwriter, mysql->user, strlen(mysql->user) + 1);//username
    if (0 == strcmp(CACHING_SHA2_PASSWORLD, mysql->plugin)) {
        uint8_t sign[SHA256_BLOCK_SIZE];
        _mysql_caching_sha2_sign(mysql, sign);
        if (BIT_CHECK(mysql->client_caps, CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA)) {
            _mysql_set_lenenc_int(&bwriter, sizeof(sign));
            binary_set_string(&bwriter, (const char *)sign, sizeof(sign));//auth_response
        } else {
            binary_set_uint8(&bwriter, (uint8_t)sizeof(sign));
            binary_set_string(&bwriter, (const char *)sign, sizeof(sign));//auth_response
        }
    } else {
        uint8_t sign[SHA1_BLOCK_SIZE];
        _mysql_native_sign(mysql, sign);
        if (BIT_CHECK(mysql->client_caps, CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA)){
            _mysql_set_lenenc_int(&bwriter, sizeof(sign));
            binary_set_string(&bwriter, (const char *)sign, sizeof(sign));//auth_response
        } else {
            binary_set_uint8(&bwriter, (uint8_t)sizeof(sign));
            binary_set_string(&bwriter, (const char *)sign, sizeof(sign));//auth_response
        }
    }
    if (BIT_CHECK(mysql->client_caps, CLIENT_CONNECT_WITH_DB)) {
        binary_set_string(&bwriter, mysql->database, strlen(mysql->database) + 1);//database
    }
    if (BIT_CHECK(mysql->client_caps, CLIENT_PLUGIN_AUTH)){
        binary_set_string(&bwriter, mysql->plugin, strlen(mysql->plugin) + 1);//client_plugin_name
    }
    if (BIT_CHECK(mysql->client_caps, CLIENT_CONNECT_ATTRS)) {
        binary_ctx battrs;
        binary_init(&battrs, NULL, 0, 0);
        _mysql_connect_attrs(&battrs);
        _mysql_set_lenenc_int(&bwriter, battrs.offset);
        binary_set_string(&bwriter, battrs.data, battrs.offset);
        FREE(battrs.data);
    }
    _set_payload_lens(&bwriter);
    ev_send(ev, mysql->fd, mysql->skid, bwriter.data, bwriter.offset, 0);
    ud->status = AUTH_PROCESS;
}
static void _mysql_ssl_exchange(mysql_ctx *mysql, ev_ctx *ev, ud_cxt *ud) {
    mysql->id++;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_skip(&bwriter, 3);
    binary_set_int8(&bwriter, mysql->id);
    binary_set_integer(&bwriter, mysql->client_caps, 4, 1);//client_flag
    binary_set_integer(&bwriter, mysql->maxpack, 4, 1);//max_packet_size
    binary_set_uint8(&bwriter, mysql->client_charset);//character_set
    binary_set_fill(&bwriter, 0, 23);//filler
    _set_payload_lens(&bwriter);
    ev_send(ev, mysql->fd, mysql->skid, bwriter.data, bwriter.offset, 0);
    ev_ssl(ev, mysql->fd, mysql->skid, 1, mysql->evssl);
    ud->status = SSL_EXCHANGE;
}
int32_t _mysql_ssl_exchanged(ev_ctx *ev, ud_cxt *ud) {
    _mysql_auth_response(ud->extra, ev, ud);
    return ERR_OK;
}
static void _mysql_auth_request(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t payload_lens;
    mysql_ctx *mysql = ud->extra;
    BIT_SET(mysql->status, LINKED);
    char *payload = _mysql_payload(mysql, buf, &payload_lens, status);
    if (NULL == payload) {
        return;
    }
    binary_ctx breader;
    binary_init(&breader, payload, payload_lens, 0);
    mysql->protocol_ver = binary_get_int8(&breader);//protocol version
    if (0x0a != mysql->protocol_ver) {
        BIT_SET(*status, PROTO_ERROR);
        FREE(payload);
        LOG_ERROR("mysql protocol version not 0x0a.");
        return;
    }
    char *val = binary_get_string(&breader, 0);//server version
    if (strlen(val) > sizeof(mysql->version) - 1) {
        BIT_SET(*status, PROTO_ERROR);
        LOG_ERROR("mysql version %s too long.", val);
        FREE(payload);
        return;
    }
    strcpy(mysql->version, val);
    mysql->thread_id = binary_get_uint32(&breader, 4, 1);//thread id
    val = binary_get_string(&breader, 8);//auth-plugin-data-part-1
    memcpy(mysql->salt, val, 8);
    binary_get_skip(&breader, 1);//filler
    mysql->server_caps = binary_get_uint16(&breader, 2, 1);//capability_flags_1
    mysql->server_charset = binary_get_uint8(&breader);
    mysql->status_flags = binary_get_uint16(&breader, 2, 1);
    mysql->server_caps |= ((uint32_t)binary_get_uint16(&breader, 2, 1) << 16);//capability_flags_2
    if (!BIT_CHECK(mysql->server_caps, CLIENT_PROTOCOL_41)
        || !BIT_CHECK(mysql->server_caps, CLIENT_PLUGIN_AUTH)) {
        BIT_SET(*status, PROTO_ERROR);
        FREE(payload);
        LOG_ERROR("CLIENT_PROTOCOL_41 or CLIENT_PLUGIN_AUTH is requred.");
        return;
    }
    if ((size_t)binary_get_uint8(&breader) != sizeof(mysql->salt) + 1) {//auth_plugin_data_len
        BIT_SET(*status, PROTO_ERROR);
        FREE(payload);
        LOG_ERROR("auth_plugin_data_len error.");
        return;
    }
    binary_get_skip(&breader, 10);//reserved
    val = binary_get_string(&breader, 13);//auth-plugin-data-part-2
    memcpy(mysql->salt + 8, val, 12);
    val = binary_get_string(&breader, 0);//auth_plugin_name
    if (strlen(val) > sizeof(mysql->plugin) - 1) {
        BIT_SET(*status, PROTO_ERROR);
        LOG_ERROR("auth plugin name %s too long.", val);
        FREE(payload);
        return;
    }
    if (0 != strcmp(val, CACHING_SHA2_PASSWORLD)
        && 0 != strcmp(val, MYSQL_NATIVE_PASSWORLD)) {
        BIT_SET(*status, PROTO_ERROR);
        LOG_ERROR("unknow auth plugin %s.", val);
        FREE(payload);
        return;
    }
    strcpy(mysql->plugin, val);
    mysql->client_caps = CLIENT_CAPS;
    if (0 != strlen(mysql->database)) {
        BIT_SET(mysql->client_caps, CLIENT_CONNECT_WITH_DB);
    }
    if (NULL != mysql->evssl
        && BIT_CHECK(mysql->server_caps, CLIENT_SSL)) {
        BIT_SET(mysql->client_caps, CLIENT_SSL);
        _mysql_ssl_exchange(mysql, ev, ud);
    } else {
        _mysql_auth_response(mysql, ev, ud);
    }
    FREE(payload);
}
static void _mysql_public_key(mysql_ctx *mysql, ev_ctx *ev) {
    mysql->id++;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_integer(&bwriter, 1, 3, 1);
    binary_set_int8(&bwriter, mysql->id);
    binary_set_uint8(&bwriter, 0x02);
    ev_send(ev, mysql->fd, mysql->skid, bwriter.data, bwriter.offset, 0);
}
static char *_mysql_password_xor_salt(mysql_ctx *mysql, size_t *lens) {
    size_t plens = strlen(mysql->password);
    *lens = plens + 1;
    char *xorpsw;
    MALLOC(xorpsw, *lens);
    memcpy(xorpsw, mysql->password, plens);
    xorpsw[plens] = '\0';
    for (size_t i = 0; i < *lens; i++) {
        xorpsw[i] ^= mysql->salt[i % sizeof(mysql->salt)];
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
        return ERR_FAILED;
    }
    //取得加密后长度
    size_t enlens;
    if (0 >= EVP_PKEY_encrypt(evpctx, NULL, &enlens, NULL, 0)) {
        EVP_PKEY_CTX_free(evpctx);
        return ERR_FAILED;
    }
    size_t offset, outlens, block_size = enlens  - 42;
    for (size_t i = 0; i < xlens; i += block_size) {
        offset = bwriter->offset;
        binary_set_skip(bwriter, enlens);
        if (0 >= EVP_PKEY_encrypt(evpctx, (unsigned char *)(bwriter->data + offset), &outlens,
            (unsigned char *)(xorpsw + i), ((i + block_size > xlens) ? (xlens - i) : block_size))) {
            EVP_PKEY_CTX_free(evpctx);
            return ERR_FAILED;
        }
    }
    EVP_PKEY_CTX_free(evpctx);
    return ERR_OK;
#else
    return ERR_FAILED;
#endif
}
static int32_t _mysql_full_auth(mysql_ctx *mysql, ev_ctx *ev, char *pubkey, size_t klens) {
    mysql->id++;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 300, 0);
    binary_set_skip(&bwriter, 3);
    binary_set_int8(&bwriter, mysql->id);
    size_t lens;
    char *xorpsw = _mysql_password_xor_salt(mysql, &lens);
    if (ERR_OK != _mysql_sha2_rsa(&bwriter, pubkey, klens, xorpsw, lens)) {
        FREE(xorpsw);
        FREE(bwriter.data);
        LOG_ERROR("_mysql_sha2_rsa error.");
        return ERR_FAILED;
    }
    FREE(xorpsw);
    _set_payload_lens(&bwriter);
    ev_send(ev, mysql->fd, mysql->skid, bwriter.data, bwriter.offset, 0);
    return ERR_OK;
}
static void _mysql_password_send(mysql_ctx *mysql, ev_ctx *ev) {
    mysql->id++;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_skip(&bwriter, 3);
    binary_set_int8(&bwriter, mysql->id);
    binary_set_string(&bwriter, mysql->password, strlen(mysql->password) + 1);
    _set_payload_lens(&bwriter);
    ev_send(ev, mysql->fd, mysql->skid, bwriter.data, bwriter.offset, 0);
}
static int32_t _mysql_auth_switch_response(mysql_ctx *mysql, ev_ctx *ev, mpack_auth_switch *auswitch) {
    if (strlen(auswitch->plugin) >= sizeof(mysql->plugin)
        || auswitch->provided.lens < sizeof(mysql->salt)
        || (0 != strcmp(auswitch->plugin, CACHING_SHA2_PASSWORLD) 
            && 0 != strcmp(auswitch->plugin, MYSQL_NATIVE_PASSWORLD))) {
        return ERR_FAILED;
    }
    mysql->id++;
    strcpy(mysql->plugin, auswitch->plugin);
    memcpy(mysql->salt, auswitch->provided.data, sizeof(mysql->salt));
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_skip(&bwriter, 3);
    binary_set_int8(&bwriter, mysql->id);
    if (0 == strcmp(CACHING_SHA2_PASSWORLD, mysql->plugin)) {
        uint8_t sign[SHA256_BLOCK_SIZE];
        _mysql_caching_sha2_sign(mysql, sign);
        binary_set_string(&bwriter, (const char *)sign, sizeof(sign));
    } else {
        uint8_t sign[SHA1_BLOCK_SIZE];
        _mysql_native_sign(mysql, sign);
        binary_set_string(&bwriter, (const char *)sign, sizeof(sign));
    }
    _set_payload_lens(&bwriter);
    ev_send(ev, mysql->fd, mysql->skid, bwriter.data, bwriter.offset, 0);
    return ERR_OK;
}
static void _mysql_auth_ok(mysql_ctx *mysql, ud_cxt *ud, int32_t *status) {
    if (ERR_OK != _hs_push(mysql->fd, mysql->skid, 1, ud, ERR_OK)) {
        BIT_SET(*status, PROTO_ERROR);
        return;
    }
    BIT_SET(mysql->status, AUTHED);
    ud->status = COMMAND;
}
static void _mysql_auth_err(binary_ctx *breader, int32_t *status) {
    mpack_err err;
    _mpack_err(breader, &err);
    BIT_SET(*status, PROTO_ERROR);
    char *errstr;
    MALLOC(errstr, err.error_msg.lens + 1);
    memcpy(errstr, err.error_msg.data, err.error_msg.lens);
    errstr[err.error_msg.lens] = '\0';
    LOG_ERROR("mysql auth error. %d %s.", err.error_code, errstr);
    FREE(errstr);
}
static void _mysql_auth_switch(mysql_ctx *mysql, ev_ctx *ev, binary_ctx *breader, int32_t *status) {
    if (BIT_CHECK(mysql->client_caps, CLIENT_PLUGIN_AUTH)) {
        mpack_auth_switch auswitch;
        auswitch.plugin = binary_get_string(breader, 0);
        auswitch.provided.lens = breader->size - breader->offset;
        auswitch.provided.data = binary_get_string(breader, auswitch.provided.lens);
        if (ERR_OK != _mysql_auth_switch_response(mysql, ev, &auswitch)) {
            BIT_SET(*status, PROTO_ERROR);
            LOG_ERROR("mysql auth switch failed.");
        }
        return;
    }
    BIT_SET(*status, PROTO_ERROR);
    LOG_ERROR("mysql auth failed.");
}
static void _mysql_caching_sha2(mysql_ctx *mysql, ev_ctx *ev, binary_ctx *breader, int32_t *status) {
    if (2 == breader->size) {
        switch (binary_get_uint8(breader)) {
        case MYSQL_CACHING_SHA2_FAST://fast auth
            break;
        case MYSQL_CACHING_SHA2_FULL://full auth
            if (BIT_CHECK(mysql->client_caps, CLIENT_SSL)) {
                _mysql_password_send(mysql, ev);
            } else {
                _mysql_public_key(mysql, ev);
            }
            break;
        default:
            BIT_SET(*status, PROTO_ERROR);
            LOG_ERROR("unknow command.");
            break;
        }
        return;
    }
    if (breader->size > 2) {
        size_t klens = breader->size - 1;
        char *pubkey = binary_get_string(breader, klens);
        if (ERR_OK != _mysql_full_auth(mysql, ev, pubkey, klens)) {
            BIT_SET(*status, PROTO_ERROR);
            LOG_ERROR("_mysql_full_auth error.");
        }
        return;
    }
    BIT_SET(*status, PROTO_ERROR);
    LOG_ERROR("unknow command.");
}
static void _mysql_auth_process(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t payload_lens;
    mysql_ctx *mysql = ud->extra;
    char *payload = _mysql_payload(mysql, buf, &payload_lens, status);
    if (NULL == payload) {
        return;
    }
    binary_ctx breader;
    binary_init(&breader, payload, payload_lens, 0);
    switch (binary_get_uint8(&breader)) {
    case MYSQL_OK:
        _mysql_auth_ok(mysql, ud, status);
        break;
    case MYSQL_ERR:
        _mysql_auth_err(&breader, status);
        break;
    case MYSQL_AUTH_SWITCH:
        _mysql_auth_switch(mysql, ev, &breader, status);
        break;
    case MYSQL_CACHING_SHA2:
        _mysql_caching_sha2(mysql, ev, &breader, status);
        break;
    default:
        BIT_SET(*status, PROTO_ERROR);
        LOG_ERROR("unknow command.");
        break;
    }
    FREE(payload);
}
static mpack_ctx *_mysql_command_process(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t payload_lens;
    mysql_ctx *mysql = ud->extra;
    char *payload = _mysql_payload(mysql, buf, &payload_lens, status);
    if (NULL == payload) {
        return NULL;
    }
    binary_ctx breader;
    binary_init(&breader, payload, payload_lens, 0);
    mpack_ctx *mpack;
    MALLOC(mpack, sizeof(mpack_ctx));
    mpack->sequence_id = mysql->id;
    mpack->payload_lens = payload_lens;
    mpack->payload = payload;
    mpack->mpack = NULL;
    mpack->_free_mpack = NULL;
    mpack->command = binary_get_uint8(&breader);
    if (ERR_OK != _mpack_parser(mysql, &breader, mpack)) {
        BIT_SET(*status, PROTO_ERROR);
        _mysql_pkfree(mpack);
        return NULL;
    }
    return mpack;
}
void *mysql_unpack(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    mpack_ctx *pack = NULL;
    switch (ud->status) {
    case INIT:
        _mysql_auth_request(ev, buf, ud, status);
        break;
    case AUTH_PROCESS:
        _mysql_auth_process(ev, buf, ud, status);
        break;
    case COMMAND:
        pack = _mysql_command_process(ev, buf, ud, status);
        break;
    default:
        BIT_SET(*status, PROTO_ERROR);
        break;
    }
    return pack;
}
int32_t mysql_init(mysql_ctx *mysql, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, const char *database, const char *charset, uint32_t maxpk) {
    if (0 == port
        || strlen(ip) > sizeof(mysql->ip) - 1
        || strlen(user) > sizeof(mysql->user) - 1
        || strlen(password) > sizeof(mysql->password) - 1
        || (NULL != database && strlen(database) > sizeof(mysql->database) - 1)) {
        return ERR_FAILED;
    }
    ZERO(mysql, sizeof(mysql_ctx));
    strcpy(mysql->ip, ip);
    strcpy(mysql->user, user);
    strcpy(mysql->password, password);
    if (NULL != database) {
        strcpy(mysql->database, database);
    }
    mysql->port = port;
    mysql->evssl = evssl;
    mysql->client_charset = _mysql_charset(charset);
    mysql->maxpack = 0 == maxpk ? ONEK * ONEK : maxpk;
    return ERR_OK;
}
int32_t mysql_try_connect(task_ctx *task, mysql_ctx *mysql) {
    if (0 != mysql->status) {
        return ERR_FAILED;
    }
    mysql->id = 0;
    mysql->cur_cmd = 0;
    mysql->fd = trigger_conn_extra(task, PACK_MYSQL, mysql, mysql->ip, mysql->port, &mysql->skid, NULL == mysql->evssl ? NETEV_NONE : NETEV_AUTHSSL);
    return INVALID_SOCK != mysql->fd ? ERR_OK : ERR_FAILED;
}
void *mysql_pack_quit(mysql_ctx *mysql, size_t *size) {
    if (!BIT_CHECK(mysql->status, LINKED)
        || !BIT_CHECK(mysql->status, AUTHED)
        || 0 != mysql->cur_cmd) {
        return NULL;
    }
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_integer(&bwriter, 1, 3, 1);
    binary_set_int8(&bwriter, 0);
    binary_set_uint8(&bwriter, MYSQL_QUIT);
    *size = bwriter.offset;
    mysql->cur_cmd = MYSQL_QUIT;
    return bwriter.data;
}
void *mysql_pack_selectdb(mysql_ctx *mysql, const char *database, size_t *size) {
    if (!BIT_CHECK(mysql->status, LINKED)
        || !BIT_CHECK(mysql->status, AUTHED)
        || 0 != mysql->cur_cmd) {
        return NULL;
    }
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_skip(&bwriter, 3);
    binary_set_int8(&bwriter, 0);
    binary_set_uint8(&bwriter, MYSQL_INIT_DB);
    binary_set_string(&bwriter, database, strlen(database));
    _set_payload_lens(&bwriter);
    *size = bwriter.offset;
    mysql->cur_cmd = MYSQL_INIT_DB;
    return bwriter.data;
}
void *mysql_pack_ping(mysql_ctx *mysql, size_t *size) {
    if (!BIT_CHECK(mysql->status, LINKED)
        || !BIT_CHECK(mysql->status, AUTHED)
        || 0 != mysql->cur_cmd) {
        return NULL;
    }
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_integer(&bwriter, 1, 3, 1);
    binary_set_int8(&bwriter, 0);
    binary_set_uint8(&bwriter, MYSQL_PING);
    *size = bwriter.offset;
    mysql->cur_cmd = MYSQL_PING;
    return bwriter.data;
}
void *mysql_pack_query(mysql_ctx *mysql, const char *sql, size_t *size) {
    if (!BIT_CHECK(mysql->status, LINKED)
        || !BIT_CHECK(mysql->status, AUTHED)
        || 0 != mysql->cur_cmd) {
        return NULL;
    }
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_skip(&bwriter, 3);
    binary_set_int8(&bwriter, 0);
    binary_set_uint8(&bwriter, MYSQL_QUERY);
    binary_set_string(&bwriter, sql, strlen(sql));
    _set_payload_lens(&bwriter);
    *size = bwriter.offset;
    mysql->cur_cmd = MYSQL_QUERY;
    return bwriter.data;
}
