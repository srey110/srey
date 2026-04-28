#include "crypt/scram.h"

scram_ctx *scram_init(const char *method, int32_t client) {
    digest_type type;
    if (0 == strcmp(method, "SCRAM-SHA-1")) {
        type = DG_SHA1;
    } else if (0 == strcmp(method, "SCRAM-SHA-256")) {
        type = DG_SHA256;
    } else if (0 == strcmp(method, "SCRAM-SHA-512")) {
        type = DG_SHA512;
    } else {
        LOG_WARN("unsupported verification methods.");
        return NULL;
    }
    scram_ctx *scram;
    CALLOC(scram, 1, sizeof(scram_ctx));
    scram->client = client;
    scram->dtype = type;
    return scram;
}
void scram_free(scram_ctx *scram) {
    if (NULL == scram) {
        return;
    }
    FREE(scram->local_first_message);
    FREE(scram->remote_first_message);
    FREE(scram->final_message_without_proof);
    FREE(scram->salt);
    FREE(scram->remote_nonce);
    FREE(scram);
}
void scram_set_user(scram_ctx *scram, const char *user) {
    if (!scram->client) {
        return;
    }
    strncpy(scram->user, user, sizeof(scram->user) - 1);
    scram->user[sizeof(scram->user) - 1] = '\0';
}
void scram_set_pwd(scram_ctx *scram, const char *pwd) {
    strncpy(scram->pwd, pwd, sizeof(scram->pwd) - 1);
    scram->pwd[sizeof(scram->pwd) - 1] = '\0';
}
void scram_set_salt(scram_ctx *scram, char *salt, size_t lens) {
    if (scram->client) {
        return;
    }
    FREE(scram->salt);
    MALLOC(scram->salt, lens);
    memcpy(scram->salt, salt, lens);
    scram->saltlen = (int32_t)lens;
}
void scram_set_iter(scram_ctx *scram, int32_t iter) {
    if (scram->client) {
        return;
    }
    scram->iter = iter;
}
const char *scram_get_user(scram_ctx *scram) {
    return scram->user;
}
// 对用户名进行转义（RFC 5802 规定 ',' 编码为 '=2C'，'=' 编码为 '=3D'）
static char *_scram_username_filter(const char *user) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    for (size_t i = 0; i < strlen(user); i++) {
        if (',' == user[i]) {
            binary_set_string(&bwriter, "=2C", strlen("=2C"));
            continue;
        }
        if ('=' == user[i]) {
            binary_set_string(&bwriter, "=3D", strlen("=3D"));
            continue;
        }
        binary_set_int8(&bwriter, user[i]);
    }
    binary_set_int8(&bwriter, 0);
    return bwriter.data;
}
// 还原经 RFC 5802 转义的用户名（'=2C' 还原为 ','，'=3D' 还原为 '='）
static char *_scram_username_recover(const char *user, size_t ulens) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    for (size_t i = 0; i < ulens;) {
        if ('=' == user[i]) {
            if (i + 3 <= ulens) {
                if (0 == memcmp(user + i, "=2C", 3)) {
                    binary_set_int8(&bwriter, ',');
                    i += 3;
                    continue;
                }
                if (0 == memcmp(user + i, "=3D", 3)) {
                    binary_set_int8(&bwriter, '=');
                    i += 3;
                    continue;
                }
            }
        }
        binary_set_int8(&bwriter, user[i]);
        i++;
    }
    binary_set_int8(&bwriter, 0);
    return bwriter.data;
}
// 在 SCRAM 消息中查找指定属性的起始位置（必须位于消息开头或逗号之后）
static char *_scram_attr_search(char *msg, size_t mlens, const char *attr) {
    char *pos = msg;
    size_t remain = mlens;
    size_t wlens = strlen(attr);
    for (;;) {
        pos = memstr(0, pos, remain, attr, wlens);
        if (NULL == pos) {
            return NULL;
        }
        if (pos == msg // 位于消息首字符
            || ',' == (pos - 1)[0]) { // 非首字符：检查前一字符是否为逗号
            return pos;
        }
        pos++;
        remain = mlens - (pos - msg);
        if (0 == remain) {
            return NULL;
        }
    }
    return NULL;
}
// 提取 SCRAM 消息中指定属性的值，通过 lens 返回值长度
static char *_scram_attr_value(char *msg, size_t mlens, const char *attr, size_t *lens) {
    char *pos = _scram_attr_search(msg, mlens, attr);
    if (NULL == pos) {
        return NULL;
    }
    char *val = pos + strlen(attr);
    size_t off = val - msg;
    pos = memstr(0, val, mlens - off, ",", 1);
    if (NULL == pos) {
        *lens = mlens - off;
    } else {
        *lens = pos - val;
    }
    return val;
}
// 计算 SaltedPassword = PBKDF2(password, salt, iter)（使用 HMAC 迭代实现）
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
// 计算 HMAC(SaltedPassword, key)，用于派生 ClientKey 或 ServerKey
static void _scram_key(scram_ctx *scram, const char *key, char result[DG_BLOCK_SIZE]) {
    hmac_ctx hmac;
    hmac_init(&hmac, scram->dtype, scram->saltedpwd, scram->hslens);
    hmac_update(&hmac, key, strlen(key));
    hmac_final(&hmac, result);
}
// 计算 H(ClientKey)，即对 ClientKey 做摘要，得到 StoredKey
static void _scram_h(scram_ctx *scram, char client_key[DG_BLOCK_SIZE], char result[DG_BLOCK_SIZE]) {
    digest_ctx digest;
    digest_init(&digest, scram->dtype);
    digest_update(&digest, client_key, scram->hslens);
    digest_final(&digest, result);
}
// 计算 HMAC(key, AuthMessage)：key 为 StoredKey 或 ServerKey，AuthMessage 为三段消息拼接
static void _scram_whole(scram_ctx *scram, char key[DG_BLOCK_SIZE], char result[DG_BLOCK_SIZE]) {
    hmac_ctx hmac;
    hmac_init(&hmac, scram->dtype, key, scram->hslens);
    if (scram->client) {
        hmac_update(&hmac, scram->local_first_message, strlen(scram->local_first_message));//n=,r= 
        hmac_update(&hmac, ",", 1);
        hmac_update(&hmac, scram->remote_first_message, strlen(scram->remote_first_message));//r=,s=,i=
        hmac_update(&hmac, ",", 1);
        hmac_update(&hmac, scram->final_message_without_proof, strlen(scram->final_message_without_proof));//c=biws,r=
    } else {
        hmac_update(&hmac, scram->remote_first_message, strlen(scram->remote_first_message));//n=,r=
        hmac_update(&hmac, ",", 1);
        hmac_update(&hmac, scram->local_first_message, strlen(scram->local_first_message));//r=,s=,i=
        hmac_update(&hmac, ",", 1);
        hmac_update(&hmac, scram->final_message_without_proof, strlen(scram->final_message_without_proof));//c=biws,r=
    }
    hmac_final(&hmac, result);
}
// 计算客户端证明（ClientProof = ClientKey XOR HMAC(StoredKey, AuthMessage)），base64 编码后写入 result
static void _scram_challenge_clientkey(scram_ctx *scram, char result[B64EN_SIZE(DG_BLOCK_SIZE)]) {
    _scram_salt_password(scram, scram->pwd);
    char clientkey[DG_BLOCK_SIZE];
    _scram_key(scram, "Client Key", clientkey);
    char storedkey[DG_BLOCK_SIZE];
    _scram_h(scram, clientkey, storedkey);
    char whole[DG_BLOCK_SIZE];
    _scram_whole(scram, storedkey, whole);
    char proof[DG_BLOCK_SIZE];
    for (int32_t i = 0; i < scram->hslens; i++) {
        proof[i] = clientkey[i] ^ whole[i];
    }
    bs64_encode(proof, scram->hslens, result);
}
// 计算服务端签名（ServerSignature = HMAC(ServerKey, AuthMessage)），base64 编码后写入 result
static void _scram_challenge_serverkey(scram_ctx *scram, char result[B64EN_SIZE(DG_BLOCK_SIZE)]) {
    char serverkey[DG_BLOCK_SIZE];
    _scram_key(scram, "Server Key", serverkey);
    char whole[DG_BLOCK_SIZE];
    _scram_whole(scram, serverkey, whole);
    bs64_encode(whole, scram->hslens, result);
}
// 客户端生成第一条消息（格式：n,,n=<user>,r=<nonce>）
static char *_scram_client_first_message(scram_ctx *scram) {
    if (SCRAM_INIT != scram->status) {
        return NULL;
    }
    char nonce[SCRAM_NONCE_LEN + 1];
    randstr(nonce, SCRAM_NONCE_LEN);
    bs64_encode(nonce, SCRAM_NONCE_LEN, scram->local_nonce);
    char *buf;
    if (EMPTYSTR(scram->user)) {
        buf = format_va("n,,n=,r=%s", scram->local_nonce);
    } else {
        size_t ulens = strlen(scram->user);
        if (NULL != memchr(scram->user, ',', ulens)
            || NULL != memchr(scram->user, '=', ulens)) {
            char *filter = _scram_username_filter(scram->user);
            buf = format_va("n,,n=%s,r=%s", filter, scram->local_nonce);
            FREE(filter);
        } else {
            buf = format_va("n,,n=%s,r=%s", scram->user, scram->local_nonce);
        }
    }
    size_t buflen = strlen(buf);
    MALLOC(scram->local_first_message, buflen - 2);//n=,r=
    memcpy(scram->local_first_message, buf + 3, buflen - 3);
    scram->local_first_message[buflen - 3] = '\0';
    scram->status = SCRAM_LOCAL_FIRST;
    return buf;
}
// 服务端解析客户端第一条消息（提取用户名和 nonce）
static int32_t _scram_parse_client_first_message(scram_ctx *scram, char *msg, size_t mlens) {
    if (SCRAM_INIT != scram->status) {
        return ERR_FAILED;
    }
    size_t lens;
    char *user = _scram_attr_value(msg, mlens, "n=", &lens);
    if (NULL == user) {
        return ERR_FAILED;
    }
    if (NULL != memstr(0, user, lens, "=2C", 3)
        || NULL != memstr(0, user, lens, "=3D", 3)) {
        user = _scram_username_recover(user, lens);
        if (strlen(user) >= sizeof(scram->user)) {
            FREE(user);
            return ERR_FAILED;
        }
        size_t ulen = strlen(user);
        memcpy(scram->user, user, ulen);
        scram->user[ulen] = '\0';
        FREE(user);
    } else {
        if (lens >= sizeof(scram->user)) {
            return ERR_FAILED;
        }
        memcpy(scram->user, user, lens);
        scram->user[lens] = '\0';
    }
    char *nonce = _scram_attr_value(msg, mlens, "r=", &lens);
    if (NULL == nonce) {
        return ERR_FAILED;
    }
    MALLOC(scram->remote_nonce, lens + 1);
    memcpy(scram->remote_nonce, nonce, lens);
    scram->remote_nonce[lens] = '\0';
    MALLOC(scram->remote_first_message, mlens - 2);//n=,r=
    memcpy(scram->remote_first_message, msg + 3, mlens - 3);
    scram->remote_first_message[mlens - 3] = '\0';
    scram->status = SCRAM_REMOTE_FIRST;
    return ERR_OK;
}
// 服务端生成第一条消息（格式：r=<client_nonce+server_nonce>,s=<salt_base64>,i=<iter>）
static char *_scram_server_first_message(scram_ctx *scram) {
    if (SCRAM_REMOTE_FIRST != scram->status) {
        return NULL;
    }
    char nonce[SCRAM_NONCE_LEN + 1];
    randstr(nonce, SCRAM_NONCE_LEN);
    bs64_encode(nonce, SCRAM_NONCE_LEN, scram->local_nonce);
    char *salt;
    MALLOC(salt, B64EN_SIZE(scram->saltlen));
    bs64_encode(scram->salt, scram->saltlen, salt);
    char *buf = format_va("r=%s%s,s=%s,i=%d", scram->remote_nonce, scram->local_nonce, salt, scram->iter);
    FREE(salt);
    size_t buflen = strlen(buf);
    MALLOC(scram->local_first_message, buflen + 1);
    memcpy(scram->local_first_message, buf, buflen);
    scram->local_first_message[buflen] = '\0';
    scram->status = SCRAM_LOCAL_FIRST;
    return buf;
}
// 客户端解析服务端第一条消息（提取 nonce、salt 和迭代轮数）
static int32_t _scram_parse_server_first_message(scram_ctx *scram, char *msg, size_t mlens) {
    if (SCRAM_LOCAL_FIRST != scram->status) {
        return ERR_FAILED;
    }
    size_t lens;
    char *nonce = _scram_attr_value(msg, mlens, "r=", &lens);
    if (NULL == nonce) {
        return ERR_FAILED;
    }
    if (lens < strlen(scram->local_nonce)
        || 0 != memcmp(nonce, scram->local_nonce, strlen(scram->local_nonce))) {
        return ERR_FAILED;
    }
    MALLOC(scram->remote_nonce, lens + 1);
    memcpy(scram->remote_nonce, nonce, lens);
    scram->remote_nonce[lens] = '\0';
    char *salt = _scram_attr_value(msg, mlens, "s=", &lens);
    if (NULL == salt) {
        return ERR_FAILED;
    }
    scram->saltlen = (int32_t)B64DE_SIZE(lens);
    MALLOC(scram->salt, scram->saltlen);
    scram->saltlen = (int32_t)bs64_decode(salt, lens, scram->salt);
    char *iter = _scram_attr_value(msg, mlens, "i=", &lens);
    if (NULL == iter) {
        return ERR_FAILED;
    }
    char ibuf[8] = { 0 };
    if (lens >= sizeof(ibuf)) {
        return ERR_FAILED;
    }
    memcpy(ibuf, iter, lens);
    scram->iter = atoi(ibuf);
    MALLOC(scram->remote_first_message, mlens + 1);
    memcpy(scram->remote_first_message, msg, mlens);
    scram->remote_first_message[mlens] = '\0';
    scram->status = SCRAM_REMOTE_FIRST;
    return ERR_OK;
}
char *scram_first_message(scram_ctx *scram) {
    return scram->client ?
        _scram_client_first_message(scram) : _scram_server_first_message(scram);
}
int32_t scram_parse_first_message(scram_ctx *scram, char *msg, size_t mlens) {
    return scram->client ?
        _scram_parse_server_first_message(scram, msg, mlens) : _scram_parse_client_first_message(scram, msg, mlens);
}
// 客户端生成最终消息（格式：c=biws,r=<nonce>,p=<ClientProof_base64>）
static char *_scram_client_final_message(scram_ctx *scram) {
    if (SCRAM_REMOTE_FIRST != scram->status) {
        return NULL;
    }
    scram->final_message_without_proof = format_va("c=biws,r=%s", scram->remote_nonce);
    char proof[B64EN_SIZE(DG_BLOCK_SIZE)];
    _scram_challenge_clientkey(scram, proof);
    scram->status = SCRAM_LOCAL_FINAL;
    return format_va("%s,p=%s", scram->final_message_without_proof, proof);
}
// 服务端验证客户端最终消息（校验 c=biws、nonce 和客户端证明）
static int32_t _scram_server_check_final_message(scram_ctx *scram, char *msg, size_t mlens) {
    if (SCRAM_LOCAL_FIRST != scram->status) {
        return ERR_FAILED;
    }
    size_t lens;
    char *biws = _scram_attr_value(msg, mlens, "c=", &lens);
    if (NULL == biws
        || strlen("biws") != lens
        || 0 != memcmp(biws, "biws", lens)) {
        return ERR_FAILED;
    }
    char *nonce = _scram_attr_value(msg, mlens, "r=", &lens);
    if (NULL == nonce) {
        return ERR_FAILED;
    }
    char *buf = format_va("%s%s", scram->remote_nonce, scram->local_nonce);
    if (strlen(buf) != lens
        || 0 != memcmp(nonce, buf, lens)) {
        FREE(buf);
        return ERR_FAILED;
    }
    char *client_proof = _scram_attr_value(msg, mlens, "p=", &lens);
    if (NULL == client_proof) {
        FREE(buf);
        return ERR_FAILED;
    }
    scram->final_message_without_proof = format_va("c=biws,r=%s", buf);
    FREE(buf);
    char proof[B64EN_SIZE(DG_BLOCK_SIZE)];
    _scram_challenge_clientkey(scram, proof);
    if (strlen(proof) != lens
        || 0 != memcmp(client_proof, proof, lens)) {
        return ERR_FAILED;
    }
    scram->status = SCRAM_REMOTE_FINAL;
    return ERR_OK;
}
// 服务端生成最终消息（格式：v=<ServerSignature_base64>）
static char *_scram_server_final_message(scram_ctx *scram) {
    if (SCRAM_REMOTE_FINAL != scram->status) {
        return NULL;
    }
    char proof[B64EN_SIZE(DG_BLOCK_SIZE)];
    _scram_challenge_serverkey(scram, proof);
    scram->status = SCRAM_LOCAL_FINAL;
    return format_va("v=%s", proof);
}
// 客户端验证服务端最终消息（检查错误字段，校验服务端签名）
static int32_t _scram_client_check_final_message(scram_ctx *scram, char *msg, size_t mlens) {
    if (SCRAM_LOCAL_FINAL != scram->status) {
        return ERR_FAILED;
    }
    size_t lens;
    char *error = _scram_attr_value(msg, mlens, "e=", &lens);
    if (NULL != error) {
        return ERR_FAILED;
    }
    char *server_proof = _scram_attr_value(msg, mlens, "v=", &lens);
    if (NULL == server_proof) {
        return ERR_FAILED;
    }
    char proof[B64EN_SIZE(DG_BLOCK_SIZE)];
    _scram_challenge_serverkey(scram, proof);
    if (strlen(proof) != lens
        || 0 != memcmp(server_proof, proof, lens)) {
        return ERR_FAILED;
    }
    scram->status = SCRAM_REMOTE_FINAL;
    return ERR_OK;
}
char *scram_final_message(scram_ctx *scram) {
    return scram->client ?
        _scram_client_final_message(scram) : _scram_server_final_message(scram);
}
int32_t scram_check_final_message(scram_ctx *scram, char *msg, size_t mlens) {
    return scram->client ?
        _scram_client_check_final_message(scram, msg, mlens) : _scram_server_check_final_message(scram, msg, mlens);
}
