#include "crypt/scram.h"
#include "utils/utils.h"

// 最小迭代轮数
#define SCRAM_MIN_ITER  4096
// 最大迭代轮数：仅用于挡住荒谬取值(解析器本身接受到 INT32_MAX，约 20 分钟 CPU)，不是安全阈值。
// 取 256 倍下限即约 100 万轮，最坏约 1 秒 worker 线程 CPU，高于 OWASP 对 PBKDF2-HMAC-SHA256
// 建议的 60 万，故任何加固过的服务端配置(如 PostgreSQL 的 scram_iterations)都能通过
#define SCRAM_MAX_ITER  (256 * SCRAM_MIN_ITER)
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
// 抹除并释放 NUL 结尾的敏感字符串。清零长度取 strlen+1,与 dup_zero / format_va 实际分配的
// lens+1 对齐,连终止符一起覆盖;顺带置空调用方字段,避免留下悬空指针
static void _scram_free_str(char **pstr) {
    if (NULL != *pstr) {
        SECURE_FREE(*pstr, strlen(*pstr) + 1);
    }
}
void scram_free(scram_ctx *scram) {
    if (NULL == scram) {
        return;
    }
    _scram_free_str(&scram->local_first_message);
    _scram_free_str(&scram->remote_first_message);
    _scram_free_str(&scram->final_message_without_proof);
    SECURE_FREE(scram->salt, (size_t)scram->saltlen);
    _scram_free_str(&scram->remote_nonce);
    SECURE_FREE(scram->cbind_data, (size_t)scram->cbind_len);
    _scram_free_str(&scram->user);
    _scram_free_str(&scram->pwd);
    SECURE_FREE(scram, sizeof(scram_ctx));
}
int32_t scram_set_user(scram_ctx *scram, const char *user, size_t ulens) {
    if (EMPTYPTR(user, ulens)) {
        return ERR_FAILED;
    }
    if (NULL != memchr(user, '\0', ulens)) {
        LOG_WARN("scram user contains embedded NUL, rejected.");
        return ERR_FAILED;
    }
    _scram_free_str(&scram->user);
    scram->user = dup_zero(user, ulens);
    return ERR_OK;
}
int32_t scram_set_pwd(scram_ctx *scram, const char *pwd, size_t plens) {
    if (NULL == pwd) {// 接受空密码""
        return ERR_FAILED;
    }
    if (NULL != memchr(pwd, '\0', plens)) {
        LOG_WARN("scram password contains embedded NUL, rejected.");
        return ERR_FAILED;
    }
    _scram_free_str(&scram->pwd);
    scram->pwd = dup_zero(pwd, plens);
    return ERR_OK;
}
int32_t scram_set_salt(scram_ctx *scram, char *salt, size_t lens) {
    if (scram->client) {
        return ERR_FAILED;
    }
    if (EMPTYPTR(salt, lens)) {
        return ERR_FAILED;
    }
    SECURE_FREE(scram->salt, (size_t)scram->saltlen);
    MALLOC(scram->salt, lens);
    memcpy(scram->salt, salt, lens);
    scram->saltlen = (int32_t)lens;
    return ERR_OK;
}
int32_t scram_set_iter(scram_ctx *scram, int32_t iter) {
    if (scram->client) {
        return ERR_FAILED;
    }
    scram->iter = iter < SCRAM_MIN_ITER ? SCRAM_MIN_ITER : iter;
    return ERR_OK;
}
int32_t scram_set_cbind(scram_ctx *scram, const char *data, size_t lens) {
    if (!scram->cbind || EMPTYPTR(data, lens)) {
        return ERR_FAILED;
    }
    SECURE_FREE(scram->cbind_data, (size_t)scram->cbind_len);
    MALLOC(scram->cbind_data, lens);
    memcpy(scram->cbind_data, data, lens);
    scram->cbind_len = (int32_t)lens;
    return ERR_OK;
}
const char *scram_get_user(scram_ctx *scram) {
    return scram->user;
}
// 对用户名进行转义（RFC 5802 规定 ',' 编码为 '=2C'，'=' 编码为 '=3D'）
static char *_scram_username_filter(const char *user) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    size_t ulen = strlen(user);
    for (size_t i = 0; i < ulen; i++) {
        if (',' == user[i]) {
            binary_set_binary(&bwriter, "=2C", 3);
            continue;
        }
        if ('=' == user[i]) {
            binary_set_binary(&bwriter, "=3D", 3);
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
    digest_free(&digest);
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
    const char *gs2 = scram->gs2_header;
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
    memcpy(scram->gs2_header, gs2, gs2_len + 1);
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
    scram->local_first_message = dup_zero(buf + gs2_len, buflen - gs2_len);
    scram->status = SCRAM_LOCAL_FIRST;
    secure_zero(nonce, sizeof(nonce));
    return buf;
}
// 剥离 client-first-message 的 GS2 头，返回 client-first-message-bare 的起始偏移，非法返回 0。
// 头格式 gs2-header = gs2-cbind-flag "," [authzid] ","(RFC 5802 §5)，flag 为 "n" / "y" / "p=<cb-name>"。
// 长度必须按实际内容定位而不能按本端配置推算：客户端合法地可以发来与本端不同的 flag，
// 按固定长度硬切会把 bare 切错位置却仍报成功，AuthMessage 随之算错、握手在证明校验处才失败。
// flag 取舍按变体分流:
//   非 PLUS 端只接受 "n"(客户端不支持 cb)与 "y"(客户端认为本端不支持);"p=" 请求的 cb 本端算不出来
//   PLUS 端只接受本端支持的那一种 cb-name。收到 "y" 意味着有人在中间抹掉了本端的 PLUS 通告,
//   即降级攻击,RFC 5802 §6 要求 fail;收到 "n" 则是选了 -PLUS 机制却不请求 cb,属协议违规
// 严格比对 cb-name 还顺带堵住一条隐患:_scram_attr_value 是在整条消息上搜 "n=" / "r=" 的,
// 若放行任意 cb-name,一个内含 "n=" 的 cb-name 就能骗过用户名提取
// authzid 必须为空:本实现没有 authorization identity 支持,静默忽略等于以与客户端所要求不同的
// 身份完成认证,比拒绝危险,所以要求第二个逗号紧跟第一个
static size_t _scram_gs2_header_lens(scram_ctx *scram, const char *msg, size_t mlens) {
    const char *c1 = memchr(msg, ',', mlens);
    if (NULL == c1) {
        return 0;
    }
    size_t flens = (size_t)(c1 - msg);
    if (scram->cbind) {
        size_t want = strlen(SCRAM_GS2_PLUS) - 2;
        if (flens != want
            || 0 != memcmp(msg, SCRAM_GS2_PLUS, want)) {
            return 0;
        }
    } else {
        if (1 != flens
            || ('n' != msg[0] && 'y' != msg[0])) {
            return 0;
        }
    }
    if (flens + 1 >= mlens
        || ',' != msg[flens + 1]) {
        return 0;
    }
    return flens + 2;
}
// 服务端解析客户端第一条消息（提取用户名和 nonce）
static int32_t _scram_parse_client_first_message(scram_ctx *scram, char *msg, size_t mlens) {
    if (SCRAM_INIT != scram->status) {
        return ERR_FAILED;
    }
    size_t gs2_len = _scram_gs2_header_lens(scram, msg, mlens);
    if (0 == gs2_len
        || mlens <= gs2_len
        || gs2_len >= sizeof(scram->gs2_header)) {
        return ERR_FAILED;
    }
    memcpy(scram->gs2_header, msg, gs2_len);
    scram->gs2_header[gs2_len] = '\0';
    size_t lens;
    char *user = _scram_attr_value(msg, mlens, "n=", &lens);
    if (NULL == user) {
        return ERR_FAILED;
    }
    if (NULL != memchr(user, '\0', lens)) {
        LOG_WARN("scram username contains embedded NUL, rejected.");
        return ERR_FAILED;
    }
    if (NULL != memstr(0, user, lens, "=2C", 3)
        || NULL != memstr(0, user, lens, "=3D", 3)) {
        char *uname = _scram_username_recover(user, lens);
        if (NULL == uname) {
            return ERR_FAILED;
        }
        int32_t setrtn = scram_set_user(scram, uname, strlen(uname));
        FREE(uname);
        if (ERR_OK != setrtn) {
            return ERR_FAILED;
        }
    } else {
        if (ERR_OK != scram_set_user(scram, user, lens)) {
            return ERR_FAILED;
        }
    }
    char *nonce = _scram_attr_value(msg, mlens, "r=", &lens);
    if (NULL == nonce) {
        return ERR_FAILED;
    }
    scram->remote_nonce = dup_zero(nonce, lens);
    // remote_first_message 保存 client-first-message-bare（GS2 头之后的部分）
    scram->remote_first_message = dup_zero(msg + gs2_len, mlens - gs2_len);
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
    scram->local_first_message = dup_zero(buf, buflen);
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
    scram->remote_nonce = dup_zero(nonce, lens);
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
    if (declens < (size_t)scram->saltlen) {
        secure_zero(scram->salt + declens, (size_t)scram->saltlen - declens);
    }
    scram->saltlen = (int32_t)declens;
    char *iter = _scram_attr_value(msg, mlens, "i=", &lens);
    if (NULL == iter) {
        return ERR_FAILED;
    }
    char ibuf[12] = { 0 };
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
        // 拒绝低于最小阈值的迭代轮数
        LOG_WARN("scram iter %d < min %d, possible downgrade attack.", scram->iter, SCRAM_MIN_ITER);
        return ERR_FAILED;
    }
    if (scram->iter > SCRAM_MAX_ITER) {
        // 拒绝过大的迭代轮数
        LOG_WARN("scram server iter %d exceeds client cap %d, raise SCRAM_MAX_ITER if the server is trusted.",
                 scram->iter, SCRAM_MAX_ITER);
        return ERR_FAILED;
    }
    scram->remote_first_message = dup_zero(msg, mlens);
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
    if (NULL == scram->pwd) {
        LOG_WARN("scram password not set.");
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
    if (NULL == scram->pwd) {
        LOG_WARN("scram password not set.");
        return ERR_FAILED;
    }
    size_t lens;
    char *cbind_val = _scram_attr_value(msg, mlens, "c=", &lens);
    if (NULL == cbind_val) {
        return ERR_FAILED;
    }
    char *cbind_b64 = _scram_cbind_b64(scram);
    int32_t mismatch = (strlen(cbind_b64) != lens || 0 != ct_memcmp(cbind_val, cbind_b64, lens));
    if (mismatch) {
        FREE(cbind_b64);
        return ERR_FAILED;
    }
    char *nonce = _scram_attr_value(msg, mlens, "r=", &lens);
    if (NULL == nonce) {
        FREE(cbind_b64);
        return ERR_FAILED;
    }
    char *buf = format_va("%s%s", scram->remote_nonce, scram->local_nonce);
    if (strlen(buf) != lens
        || 0 != ct_memcmp(nonce, buf, lens)) {
        FREE(cbind_b64);
        FREE(buf);
        return ERR_FAILED;
    }
    char *client_proof = _scram_attr_value(msg, mlens, "p=", &lens);
    if (NULL == client_proof) {
        FREE(cbind_b64);
        FREE(buf);
        return ERR_FAILED;
    }
    scram->final_message_without_proof = format_va("c=%s,r=%s", cbind_b64, buf);
    FREE(cbind_b64);
    FREE(buf);
    char proof[B64EN_SIZE(DG_BLOCK_SIZE)];
    _scram_challenge_clientkey(scram, proof);
    if (strlen(proof) != lens
        || 0 != ct_memcmp(client_proof, proof, lens)) {
        secure_zero(proof, sizeof(proof));
        _scram_free_str(&scram->final_message_without_proof);
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
