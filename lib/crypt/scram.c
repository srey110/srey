#include "crypt/scram.h"
#include "utils/utils.h"

/* PBKDF2 最小迭代轮数（RFC 5802 推荐 >= 4096）。
 * 客户端解析服务端 i= 字段时，低于此值视为降级攻击，拒绝握手。*/
#define SCRAM_MIN_ITER  4096
/* GS2 头：标准变体不声明 channel binding；PLUS 变体使用 tls-server-end-point 绑定类型。*/
#define SCRAM_GS2_STD   "n,,"
#define SCRAM_GS2_PLUS  "p=tls-server-end-point,,"

scram_ctx *scram_init(const char *method, int32_t client) {
    digest_type type;
    int32_t cbind = 0;
    if (0 == strcmp(method, "SCRAM-SHA-1")) {
        type = DG_SHA1;
    } else if (0 == strcmp(method, "SCRAM-SHA-1-PLUS")) {
        type = DG_SHA1;
        cbind = 1;
    } else if (0 == strcmp(method, "SCRAM-SHA-256")) {
        type = DG_SHA256;
    } else if (0 == strcmp(method, "SCRAM-SHA-256-PLUS")) {
        type = DG_SHA256;
        cbind = 1;
    } else if (0 == strcmp(method, "SCRAM-SHA-512")) {
        type = DG_SHA512;
    } else if (0 == strcmp(method, "SCRAM-SHA-512-PLUS")) {
        type = DG_SHA512;
        cbind = 1;
    } else {
        LOG_WARN("unsupported verification methods.");
        return NULL;
    }
    scram_ctx *scram;
    CALLOC(scram, 1, sizeof(scram_ctx));
    scram->client = client;
    scram->dtype = type;
    scram->cbind = cbind;
    return scram;
}
void scram_free(scram_ctx *scram) {
    if (NULL == scram) {
        return;
    }
    if (NULL != scram->local_first_message) {
        secure_zero(scram->local_first_message, strlen(scram->local_first_message));
    }
    FREE(scram->local_first_message);
    if (NULL != scram->remote_first_message) {
        secure_zero(scram->remote_first_message, strlen(scram->remote_first_message));
    }
    FREE(scram->remote_first_message);
    if (NULL != scram->final_message_without_proof) {
        secure_zero(scram->final_message_without_proof, strlen(scram->final_message_without_proof));
    }
    FREE(scram->final_message_without_proof);
    if (NULL != scram->salt) {
        secure_zero(scram->salt, (size_t)scram->saltlen);
    }
    FREE(scram->salt);
    if (NULL != scram->remote_nonce) {
        secure_zero(scram->remote_nonce, strlen(scram->remote_nonce));
    }
    FREE(scram->remote_nonce);
    if (NULL != scram->cbind_data) {
        secure_zero(scram->cbind_data, (size_t)scram->cbind_len);
    }
    FREE(scram->cbind_data);
    secure_zero(scram, sizeof(scram_ctx));
    FREE(scram);
}
void scram_set_user(scram_ctx *scram, const char *user) {
    if (!scram->client) {
        return;
    }
    if (strlen(user) >= sizeof(scram->user)) {
        LOG_WARN("scram user name too long, truncated.");
    }
    secure_zero(scram->user, sizeof(scram->user));
    safe_fill_str(scram->user, sizeof(scram->user), user);
}
int32_t scram_set_pwd(scram_ctx *scram, const char *pwd) {
    size_t plen = strlen(pwd);
    if (plen >= sizeof(scram->pwd)) {
        LOG_ERROR("scram password too long (%zu >= %zu), rejected.", plen, sizeof(scram->pwd));
        return ERR_FAILED;
    }
    secure_zero(scram->pwd, sizeof(scram->pwd));
    safe_fill_str(scram->pwd, sizeof(scram->pwd), pwd);
    return ERR_OK;
}
void scram_set_salt(scram_ctx *scram, char *salt, size_t lens) {
    if (scram->client) {
        return;
    }
    if (EMPTYPTR(salt, lens)) {
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
    scram->iter = iter < SCRAM_MIN_ITER ? SCRAM_MIN_ITER : iter;
}
void scram_set_cbind(scram_ctx *scram, const char *data, size_t lens) {
    if (!scram->cbind || EMPTYPTR(data, lens)) {
        return;
    }
    FREE(scram->cbind_data);
    MALLOC(scram->cbind_data, lens);
    memcpy(scram->cbind_data, data, lens);
    scram->cbind_len = (int32_t)lens;
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
            binary_set_binary(&bwriter, "=2C", strlen("=2C"));
            continue;
        }
        if ('=' == user[i]) {
            binary_set_binary(&bwriter, "=3D", strlen("=3D"));
            continue;
        }
        binary_set_int8(&bwriter, user[i]);
    }
    binary_set_int8(&bwriter, 0);
    return bwriter.data;
}
// 还原经 RFC 5802 转义的用户名（'=2C' 还原为 ','，'=3D' 还原为 '='）
// RFC 5802 §5.1：'=' 后只能跟 "2C" 或 "3D"，其他 =XX 视为非法 SASLname 返 NULL
static char *_scram_username_recover(const char *user, size_t ulens) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    for (size_t i = 0; i < ulens;) {
        if ('=' == user[i]) {
            if (i + 3 > ulens) {
                binary_free(&bwriter);
                return NULL;
            }
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
            binary_free(&bwriter);
            return NULL;
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
    secure_zero(hash, sizeof(hash));
    hmac_free(&hmac);
}
// 计算 HMAC(SaltedPassword, key)，用于派生 ClientKey 或 ServerKey
static void _scram_key(scram_ctx *scram, const char *key, char result[DG_BLOCK_SIZE]) {
    hmac_ctx hmac;
    hmac_init(&hmac, scram->dtype, scram->saltedpwd, scram->hslens);
    hmac_update(&hmac, key, strlen(key));
    hmac_final(&hmac, result);
    hmac_free(&hmac);
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
        hmac_update(&hmac, scram->final_message_without_proof, strlen(scram->final_message_without_proof));//c=...,r=
    } else {
        hmac_update(&hmac, scram->remote_first_message, strlen(scram->remote_first_message));//n=,r=
        hmac_update(&hmac, ",", 1);
        hmac_update(&hmac, scram->local_first_message, strlen(scram->local_first_message));//r=,s=,i=
        hmac_update(&hmac, ",", 1);
        hmac_update(&hmac, scram->final_message_without_proof, strlen(scram->final_message_without_proof));//c=...,r=
    }
    hmac_final(&hmac, result);
    hmac_free(&hmac);
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
    secure_zero(clientkey, sizeof(clientkey));
    secure_zero(storedkey, sizeof(storedkey));
    secure_zero(whole, sizeof(whole));
    secure_zero(proof, sizeof(proof));
}
// 计算服务端签名（ServerSignature = HMAC(ServerKey, AuthMessage)），base64 编码后写入 result
static void _scram_challenge_serverkey(scram_ctx *scram, char result[B64EN_SIZE(DG_BLOCK_SIZE)]) {
    char serverkey[DG_BLOCK_SIZE];
    _scram_key(scram, "Server Key", serverkey);
    char whole[DG_BLOCK_SIZE];
    _scram_whole(scram, serverkey, whole);
    bs64_encode(whole, scram->hslens, result);
    secure_zero(serverkey, sizeof(serverkey));
    secure_zero(whole, sizeof(whole));
}
// 生成 c= 字段的 base64 值：base64(GS2头 + channel_binding_data)，调用方负责释放
static char *_scram_cbind_b64(scram_ctx *scram) {
    const char *gs2 = scram->cbind ? SCRAM_GS2_PLUS : SCRAM_GS2_STD;
    size_t gs2_len = strlen(gs2);
    size_t data_len = (scram->cbind && NULL != scram->cbind_data) ? (size_t)scram->cbind_len : 0;
    size_t total = gs2_len + data_len;
    char *input;
    MALLOC(input, total);
    memcpy(input, gs2, gs2_len);
    if (data_len > 0) {
        memcpy(input + gs2_len, scram->cbind_data, data_len);
    }
    char *b64;
    MALLOC(b64, B64EN_SIZE(total));
    bs64_encode(input, total, b64);
    FREE(input);
    return b64;
}
// 客户端生成第一条消息（格式：[GS2]n=<user>,r=<nonce>）
static char *_scram_client_first_message(scram_ctx *scram) {
    if (SCRAM_INIT != scram->status) {
        return NULL;
    }
    char nonce[SCRAM_NONCE_LEN];
    /* 使用 OS 级 CSPRNG 生成随机 nonce，避免伪随机数发生器种子可预测的风险。*/
    if (ERR_OK != csprng_rand(nonce, SCRAM_NONCE_LEN)) {
        return NULL;
    }
    bs64_encode(nonce, SCRAM_NONCE_LEN, scram->local_nonce);
    const char *gs2 = scram->cbind ? SCRAM_GS2_PLUS : SCRAM_GS2_STD;
    size_t gs2_len = strlen(gs2);
    char *buf;
    if (EMPTYSTR(scram->user)) {
        buf = format_va("%sn=,r=%s", gs2, scram->local_nonce);
    } else {
        size_t ulens = strlen(scram->user);
        if (NULL != memchr(scram->user, ',', ulens)
            || NULL != memchr(scram->user, '=', ulens)) {
            char *filter = _scram_username_filter(scram->user);
            buf = format_va("%sn=%s,r=%s", gs2, filter, scram->local_nonce);
            FREE(filter);
        } else {
            buf = format_va("%sn=%s,r=%s", gs2, scram->user, scram->local_nonce);
        }
    }
    size_t buflen = strlen(buf);
    // local_first_message 保存 client-first-message-bare（GS2 头之后的部分）
    MALLOC(scram->local_first_message, buflen - gs2_len + 1);
    memcpy(scram->local_first_message, buf + gs2_len, buflen - gs2_len);
    scram->local_first_message[buflen - gs2_len] = '\0';
    scram->status = SCRAM_LOCAL_FIRST;
    secure_zero(nonce, sizeof(nonce));
    return buf;
}
// 服务端解析客户端第一条消息（提取用户名和 nonce）
static int32_t _scram_parse_client_first_message(scram_ctx *scram, char *msg, size_t mlens) {
    const char *gs2 = scram->cbind ? SCRAM_GS2_PLUS : SCRAM_GS2_STD;
    size_t gs2_len = strlen(gs2);
    if (SCRAM_INIT != scram->status
        || mlens <= gs2_len) {
        return ERR_FAILED;
    }
    size_t lens;
    char *user = _scram_attr_value(msg, mlens, "n=", &lens);
    if (NULL == user) {
        return ERR_FAILED;
    }
    if (NULL != memstr(0, user, lens, "=2C", 3)
        || NULL != memstr(0, user, lens, "=3D", 3)) {
        char *recovered = _scram_username_recover(user, lens);
        if (NULL == recovered) {
            // 非法 =XX 转义被拒
            return ERR_FAILED;
        }
        if (strlen(recovered) >= sizeof(scram->user)) {
            FREE(recovered);
            return ERR_FAILED;
        }
        safe_fill_str(scram->user, sizeof(scram->user), recovered);
        FREE(recovered);
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
    // remote_first_message 保存 client-first-message-bare（GS2 头之后的部分）
    MALLOC(scram->remote_first_message, mlens - gs2_len + 1);
    memcpy(scram->remote_first_message, msg + gs2_len, mlens - gs2_len);
    scram->remote_first_message[mlens - gs2_len] = '\0';
    scram->status = SCRAM_REMOTE_FIRST;
    return ERR_OK;
}
// 服务端生成第一条消息（格式：r=<client_nonce+server_nonce>,s=<salt_base64>,i=<iter>）
static char *_scram_server_first_message(scram_ctx *scram) {
    if (SCRAM_REMOTE_FIRST != scram->status) {
        return NULL;
    }
    char nonce[SCRAM_NONCE_LEN];
    /* 使用 OS 级 CSPRNG 生成随机 nonce，避免伪随机数发生器种子可预测的风险。*/
    if (ERR_OK != csprng_rand(nonce, SCRAM_NONCE_LEN)) {
        return NULL;
    }
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
    secure_zero(nonce, sizeof(nonce));
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
    size_t declens = bs64_decode(salt, lens, scram->salt);
    if (0 == declens) {
        return ERR_FAILED;
    }
    scram->saltlen = (int32_t)declens;
    char *iter = _scram_attr_value(msg, mlens, "i=", &lens);
    if (NULL == iter) {
        return ERR_FAILED;
    }
    char ibuf[8] = { 0 };
    if (lens >= sizeof(ibuf)) {
        return ERR_FAILED;
    }
    memcpy(ibuf, iter, lens);
    // strtol 严格解析：拒绝空白前缀 / 尾随垃圾 / 溢出 / 负数
    char *endptr;
    errno = 0;
    long val = strtol(ibuf, &endptr, 10);
    if (endptr == ibuf || '\0' != *endptr || 0 != errno || val < 0 || val > INT32_MAX) {
        LOG_WARN("scram iter parse failed: '%s'.", ibuf);
        return ERR_FAILED;
    }
    scram->iter = (int32_t)val;
    if (scram->iter < SCRAM_MIN_ITER) {
        /* 拒绝低于最小阈值的迭代轮数，防止恶意服务端通过 i=1 等小值
         * 将 PBKDF2 降级为单次 HMAC，导致密码暴力破解成本骤降。*/
        LOG_WARN("scram iter %d < min %d, possible downgrade attack.", scram->iter, SCRAM_MIN_ITER);
        return ERR_FAILED;
    }
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
    int32_t rtn = scram->client ?
        _scram_parse_server_first_message(scram, msg, mlens) : _scram_parse_client_first_message(scram, msg, mlens);
    if (ERR_OK != rtn) {
        scram->status = SCRAM_ERROR;
    }
    return rtn;
}
// 客户端生成最终消息（格式：c=<cbind_b64>,r=<nonce>,p=<ClientProof_base64>）
static char *_scram_client_final_message(scram_ctx *scram) {
    if (SCRAM_REMOTE_FIRST != scram->status) {
        return NULL;
    }
    char *cbind_b64 = _scram_cbind_b64(scram);
    scram->final_message_without_proof = format_va("c=%s,r=%s", cbind_b64, scram->remote_nonce);
    FREE(cbind_b64);
    char proof[B64EN_SIZE(DG_BLOCK_SIZE)];
    _scram_challenge_clientkey(scram, proof);
    scram->status = SCRAM_LOCAL_FINAL;
    char *buf = format_va("%s,p=%s", scram->final_message_without_proof, proof);
    secure_zero(proof, sizeof(proof));
    return buf;
}
// 服务端验证客户端最终消息（校验 c= 值、nonce 和客户端证明）
static int32_t _scram_server_check_final_message(scram_ctx *scram, char *msg, size_t mlens) {
    if (SCRAM_LOCAL_FIRST != scram->status) {
        return ERR_FAILED;
    }
    size_t lens;
    char *cbind_val = _scram_attr_value(msg, mlens, "c=", &lens);
    if (NULL == cbind_val) {
        return ERR_FAILED;
    }
    char *cbind_b64 = _scram_cbind_b64(scram);
    int32_t mismatch = (strlen(cbind_b64) != lens || 0 != ct_memcmp(cbind_val, cbind_b64, lens));
    FREE(cbind_b64);
    if (mismatch) {
        return ERR_FAILED;
    }
    char *nonce = _scram_attr_value(msg, mlens, "r=", &lens);
    if (NULL == nonce) {
        return ERR_FAILED;
    }
    char *buf = format_va("%s%s", scram->remote_nonce, scram->local_nonce);
    if (strlen(buf) != lens
        || 0 != ct_memcmp(nonce, buf, lens)) {
        FREE(buf);
        return ERR_FAILED;
    }
    char *client_proof = _scram_attr_value(msg, mlens, "p=", &lens);
    if (NULL == client_proof) {
        FREE(buf);
        return ERR_FAILED;
    }
    cbind_b64 = _scram_cbind_b64(scram);
    scram->final_message_without_proof = format_va("c=%s,r=%s", cbind_b64, buf);
    FREE(cbind_b64);
    FREE(buf);
    char proof[B64EN_SIZE(DG_BLOCK_SIZE)];
    _scram_challenge_clientkey(scram, proof);
    if (strlen(proof) != lens
        || 0 != ct_memcmp(client_proof, proof, lens)) {
        secure_zero(proof, sizeof(proof));
        return ERR_FAILED;
    }
    secure_zero(proof, sizeof(proof));
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
    char *buf = format_va("v=%s", proof);
    secure_zero(proof, sizeof(proof));
    return buf;
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
        || 0 != ct_memcmp(server_proof, proof, lens)) {
        secure_zero(proof, sizeof(proof));
        return ERR_FAILED;
    }
    secure_zero(proof, sizeof(proof));
    scram->status = SCRAM_REMOTE_FINAL;
    return ERR_OK;
}
char *scram_final_message(scram_ctx *scram) {
    return scram->client ?
        _scram_client_final_message(scram) : _scram_server_final_message(scram);
}
int32_t scram_check_final_message(scram_ctx *scram, char *msg, size_t mlens) {
    int32_t rtn = scram->client ?
        _scram_client_check_final_message(scram, msg, mlens) : _scram_server_check_final_message(scram, msg, mlens);
    if (ERR_OK != rtn) {
        scram->status = SCRAM_ERROR;
    }
    return rtn;
}
