#include "protocol/pgsql/pgsql.h"
#include "protocol/pgsql/pgsql_parse.h"
#include "crypt/scram.h"
#include "crypt/md5.h"
#include "event/event.h"
#include "utils/utils.h"

//https://www.postgresql.org/docs/18/protocol-flow.html
//https://pg.center/docs/18/protocol.html

// 连接状态枚举
typedef enum parse_status {
    INIT = 0,   // 初始状态，等待 SSL 协商响应
    AUTH,       // 认证阶段，等待认证消息
    COMMAND     // 命令阶段，正常收发命令
}parse_status;

// 支持的 SASL 认证方法列表（优先级从高到低：PLUS变体优先，回退到无通道绑定版本）
#if WITH_SSL
static const char *_pgsql_scram_mod[] = {
    "SCRAM-SHA-256-PLUS",  // 带通道绑定，首选
    "SCRAM-SHA-256",       // 无通道绑定，回退
};
#else
static const char *_pgsql_scram_mod[] = { "SCRAM-SHA-256" };
#endif
static _handshaked_push _hs_push;                             // 握手完成推送回调函数指针

void _pgsql_init(void *hspush) {
    _hs_push = hspush;
}
void _pgsql_pkfree(void *pack) {
    _pgpack_free(pack);
}
void _pgsql_udfree(ud_cxt *ud) {
    if (NULL == ud->context) {
        return;
    }
    pgsql_ctx *pg = (pgsql_ctx *)ud->context;
    _pgsql_pkfree(pg->pack);
    pg->pack = NULL;
    scram_free(pg->scram);
    pg->scram = NULL;
    pg->sk.fd = INVALID_SOCK;
    ud->context = NULL;
}
void _pgsql_closed(ud_cxt *ud) {
    _pgsql_udfree(ud);
}
int32_t _pgsql_may_resume(void *data) {
    if (NULL == data) {
        return ERR_OK;
    }
    pgpack_ctx *pgpack = data;
    // 通知消息由上层异步处理，不允许当前协程立即恢复
    if (PGPACK_NOTIFICATION == pgpack->type) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 连接建立后请求是否启用 SSL 加密
int32_t _pgsql_on_connected(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud, int32_t err) {
    (void)ud;
    if (ERR_OK != err) {
        return err;
    }
    char buf[8];
    pack_integer(buf, 8, 4, 0);         // 消息总长度 = 8
    pack_integer(buf + 4, 80877103, 4, 0); // SSLRequest 魔数
    return ev_send(ev, fd, skid, buf, sizeof(buf), 1);
}
// 发送 Startup 消息，包含用户名、数据库名等启动参数
static int32_t _pgsql_startup(ev_ctx *ev, ud_cxt *ud) {
    pgsql_ctx *pg = (pgsql_ctx *)ud->context;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_skip(&bwriter, 4);                           // 预留长度字段
    binary_set_integer(&bwriter, 3, 2, 0);                  // 协议主版本号 3
    binary_set_integer(&bwriter, 0, 2, 0);                  // 协议次版本号 0
    binary_set_string(&bwriter, "user");
    binary_set_string(&bwriter, pg->user);
    binary_set_string(&bwriter, "database");
    binary_set_string(&bwriter, pg->database);
    binary_set_string(&bwriter, "application_name");
    binary_set_string(&bwriter, "srey");
    binary_set_int8(&bwriter, 0);                           // 参数列表结束标志
    size_t size = bwriter.offset;
    binary_offset(&bwriter, 0);
    binary_set_integer(&bwriter, size, 4, 0);               // 回填消息总长度
    ud->status = AUTH;
    return ev_send(ev, pg->sk.fd, pg->sk.skid, bwriter.data, size, 0);
}
// 处理服务端 SSL 响应：'S' 升级为 SSL，'N' 直接发送 Startup 消息
static void _pgsql_ssl_response(pgsql_ctx *pg, ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (1 > buffer_size(buf)) {
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    char ssl[1];
    ASSERTAB(sizeof(ssl) == buffer_remove(buf, ssl, sizeof(ssl)), "copy buffer failed.");
    switch (ssl[0]) {
    case 'S':
        // 服务端支持 SSL；若客户端未配置 evssl 则状态不一致（已发 SSLRequest 却无 evssl 上下文可升级），拒收
        if (NULL != pg->evssl) {
            if (ERR_OK != ev_ssl(ev, pg->sk.fd, pg->sk.skid, 1, pg->evssl)) {
                BIT_SET(*status, PROT_ERROR);
            }
        } else {
            BIT_SET(*status, PROT_ERROR);
            LOG_ERROR("server agreed to SSL but client evssl is not configured.");
        }
        break;
    case 'N':
        // 服务端不支持 SSL；若客户端配置 evssl 即视为强制 SSL，必须拒收，
        // 杜绝 MITM 在明文 SSLRequest 响应中把 'S' 改写为 'N' 强制降级到明文 Startup（含 AuthCleartextPassword 直发明文密码）
        if (NULL != pg->evssl) {
            BIT_SET(*status, PROT_ERROR);
            LOG_ERROR("server refused SSL but client requires SSL.");
            break;
        }
        if (ERR_OK != _pgsql_startup(ev, ud)) {
            BIT_SET(*status, PROT_ERROR);
        }
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        break;
    }
}
// SSL 握手完成后提取服务端证书 SHA-256 摘要（用于 SCRAM-PLUS 通道绑定），然后发送 Startup 消息
int32_t _pgsql_ssl_exchanged(ev_ctx *ev, ud_cxt *ud, void *ssl) {
#if WITH_SSL
    if (NULL != ssl) {
        pgsql_ctx *pg = (pgsql_ctx *)ud->context;
        X509 *cert = SSL_get_peer_certificate((SSL *)ssl);
        if (NULL != cert) {
            unsigned int len = 0;
            // tls-server-end-point 定义为服务端证书的 SHA-256 摘要（RFC 5929）
            X509_digest(cert, EVP_sha256(), (unsigned char *)pg->tls_cbind, &len);
            pg->tls_cbind_len = (int32_t)len;
            X509_free(cert);
        }
    }
#else
    (void)ssl;
#endif
    return _pgsql_startup(ev, ud);
}
// 从接收缓冲区读取一个完整的 pgsql 消息（含类型码+长度+数据），返回堆上的数据指针
static char *_pgsql_payload(buffer_ctx *buf, int32_t *lens, int32_t *status) {
    size_t blens = buffer_size(buf);
    if (5 > blens) {
        // 数据不足一个完整消息头（1字节类型码 + 4字节长度）
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    ASSERTAB((size_t)sizeof(*lens) == buffer_copyout(buf, 1, lens, sizeof(*lens)), "copy buffer failed.");
    *lens = (int32_t)unpack_integer((const char*)lens, 4, 0, 0);
    if (*lens < 4) {
        // pgsql 协议规定 length 字段含自身 4 字节，合法值 ≥ 4；非法值会让后续解析下溢/越界
        BIT_SET(*status, PROT_ERROR);
        return NULL;
    }
    uint32_t total = (uint32_t)(*lens) + 1; // 消息总长度 = 类型码(1) + 消息体(lens)
    if ((size_t)total > blens) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    char *pack;
    MALLOC(pack, total);
    ASSERTAB(total == (uint32_t)buffer_remove(buf, pack, total), "copy buffer failed.");
    return pack;
}
// 从服务端 SASL 方法列表中按优先级选择本端支持的认证方法
// WITH_SSL 时优先选 SCRAM-SHA-256-PLUS（需要 tls_cbind_len > 0），回退到 SCRAM-SHA-256
static const char *_pgsql_get_authmod(pgsql_ctx *pg, binary_ctx *breader) {
#if !WITH_SSL
    (void)pg;
#endif
    // 先将服务端方法列表收集到栈数组（最多 16 个）
    const char *server_mods[16];
    size_t server_count = 0;
    size_t remain;
    size_t slen;
    while (server_count < ARRAY_SIZE(server_mods)) {
        remain = breader->size - breader->offset;
        if (remain < 2) {
            break;
        }
        slen = strnlen(breader->data + breader->offset, remain);
        if (slen >= remain) {
            break;
        }
        server_mods[server_count] = binary_get_string(breader);
        server_count++;
    }
    // 按优先级遍历本端支持的方法列表，找到服务端也支持的第一个
    size_t n = ARRAY_SIZE(_pgsql_scram_mod);
    for (size_t i = 0; i < n; i++) {
        if (NULL == _pgsql_scram_mod[i]) {
            continue;
        }
#if WITH_SSL
        // PLUS 变体须有通道绑定数据，否则跳过
        if (pg->tls_cbind_len <= 0
            && NULL != strstr(_pgsql_scram_mod[i], "-PLUS")) {
            continue;
        }
#endif
        for (size_t j = 0; j < server_count; j++) {
            if (0 == strcmp(_pgsql_scram_mod[i], server_mods[j])) {
                return _pgsql_scram_mod[i];
            }
        }
    }
    return NULL;
}
// 明文密码认证（AuthenticationCleartextPassword / GSSResponse）
static int32_t _pgsql_password_auth(pgsql_ctx *pg, ev_ctx *ev) {
    binary_ctx bwriter;
    pgsql_pack_start(&bwriter, 'p');
    binary_set_string(&bwriter, pg->password);
    pgsql_pack_end(&bwriter);
    return ev_send(ev, pg->sk.fd, pg->sk.skid, bwriter.data, bwriter.offset, 0);
}
// MD5 密码认证（AuthenticationMD5Password）
// 响应格式："md5" + hex(md5(hex(md5(password+user)) + salt)) + '\0'
static int32_t _pgsql_md5_auth(pgsql_ctx *pg, ev_ctx *ev, binary_ctx *breader) {
    // AuthenticationMD5Password 消息体：Int32(认证码 5) + Byte4(salt)，salt 须恰好 4 字节
    if (breader->size - breader->offset < 4) {
        LOG_WARN("%s", "md5 auth: salt length invalid.");
        return ERR_FAILED;
    }
    const char *salt = binary_get_binary(breader, 4);
    md5_ctx md5;
    char hash[MD5_BLOCK_SIZE];
    char inner_hex[MD5_BLOCK_SIZE * 2 + 1];       // hex(md5(password+user))
    char response[3 + MD5_BLOCK_SIZE * 2 + 1];    // "md5" + hex + '\0'
    // 第一步：内层 md5(password + user) → 十六进制字符串
    md5_init(&md5);
    md5_update(&md5, pg->password, strlen(pg->password));
    md5_update(&md5, pg->user, strlen(pg->user));
    md5_final(&md5, hash);
    for (int32_t i = 0; i < MD5_BLOCK_SIZE; i++) {
        SNPRINTF(inner_hex + i * 2, 3, "%02x", (uint8_t)hash[i]);
    }
    inner_hex[MD5_BLOCK_SIZE * 2] = '\0';
    // 第二步：外层 md5(inner_hex + salt) → 十六进制字符串，拼接 "md5" 前缀
    md5_init(&md5);
    md5_update(&md5, inner_hex, MD5_BLOCK_SIZE * 2);
    md5_update(&md5, salt, 4);
    md5_final(&md5, hash);
    safe_fill_str(response, sizeof(response), "md5");
    for (int32_t i = 0; i < MD5_BLOCK_SIZE; i++) {
        SNPRINTF(response + 3 + i * 2, 3, "%02x", (uint8_t)hash[i]);
    }
    response[3 + MD5_BLOCK_SIZE * 2] = '\0';
    secure_zero(hash, sizeof(hash));
    secure_zero(inner_hex, sizeof(inner_hex));
    secure_zero(&md5, sizeof(md5));
    binary_ctx bwriter;
    pgsql_pack_start(&bwriter, 'p');
    binary_set_string(&bwriter, response);
    secure_zero(response, sizeof(response));
    pgsql_pack_end(&bwriter);
    return ev_send(ev, pg->sk.fd, pg->sk.skid, bwriter.data, bwriter.offset, 0);
}
// SCRAM 第一步：发送 client-first-message（SCRAM-SHA-256 或 SCRAM-SHA-256-PLUS）
static int32_t _pgsql_scram_client_first(pgsql_ctx *pg, ev_ctx *ev, const char *mod) {
    // server 异常重发 0x0a（或 0x0a 后未走完 0x0b/0x0c 又来一次 0x0a）时，
    // 旧 scram_ctx 必须先释放再覆盖；scram_free 自守 NULL，首次进入安全
    scram_free(pg->scram);
    pg->scram = scram_init(mod, 1);
    if (NULL == pg->scram) {
        return ERR_FAILED;
    }
    scram_set_user(pg->scram, pg->user, strlen(pg->user));
#if WITH_SSL
    // PLUS 变体：注入 tls-server-end-point 通道绑定数据
    if (pg->scram->cbind && pg->tls_cbind_len > 0) {
        scram_set_cbind(pg->scram, pg->tls_cbind, (size_t)pg->tls_cbind_len);
    }
#endif
    char *first_message = scram_first_message(pg->scram);
    if (NULL == first_message) {
        return ERR_FAILED;
    }
    binary_ctx bwriter;
    pgsql_pack_start(&bwriter, 'p');
    binary_set_string(&bwriter, mod);                    // 所选 SASL 机制名称
    size_t fmlens = strlen(first_message);
    binary_set_integer(&bwriter, fmlens, 4, 0);             // client-first-message 长度
    binary_set_binary(&bwriter, first_message, fmlens);
    FREE(first_message);
    pgsql_pack_end(&bwriter);
    return ev_send(ev, pg->sk.fd, pg->sk.skid, bwriter.data, bwriter.offset, 0);
}
// SCRAM-SHA-256 第二步：解析 server-first-message 并发送 client-final-message
static int32_t _pgsql_scram_client_final(pgsql_ctx *pg, ev_ctx *ev, binary_ctx *breader) {
    // server 必须先经 0x0a 初始化 pg->scram 才允许进入 0x0b；
    // 乱序消息（恶意 server / MITM 在明文阶段注入）裸调 scram API 会解引用 NULL
    if (NULL == pg->scram) {
        LOG_WARN("received SASLContinue without prior SASL Initial.");
        return ERR_FAILED;
    }
    if (ERR_OK != scram_parse_first_message(pg->scram,
        breader->data + breader->offset, breader->size - breader->offset)) {
        return ERR_FAILED;
    }
    scram_set_pwd(pg->scram, pg->password, strlen(pg->password));
    char *final_message = scram_final_message(pg->scram);
    if (NULL == final_message) {
        return ERR_FAILED;
    }
    binary_ctx bwriter;
    pgsql_pack_start(&bwriter, 'p');
    binary_set_binary(&bwriter, final_message, strlen(final_message));
    FREE(final_message);
    pgsql_pack_end(&bwriter);
    return ev_send(ev, pg->sk.fd, pg->sk.skid, bwriter.data, bwriter.offset, 0);
}
// 根据认证类型码分派具体的认证处理逻辑
static void _pgsql_auth_process(pgsql_ctx *pg, ev_ctx *ev, binary_ctx *breader, int32_t *status) {
    int32_t code = (int32_t)binary_get_integer(breader, 4, 0);
    switch (code) {
    case 0x00: // AuthenticationOk：认证成功，释放 SCRAM 上下文
        scram_free(pg->scram);
        pg->scram = NULL;
        break;
    case 0x03: // AuthenticationCleartextPassword：明文密码认证
        if (NULL == pg->evssl) {
            LOG_WARN("cleartext password over non-SSL connection.");
        }
        if (ERR_OK != _pgsql_password_auth(pg, ev)) {
            BIT_SET(*status, PROT_ERROR);
        }
        break;
    case 0x05: // AuthenticationMD5Password：MD5 密码认证，salt 为紧随认证码的 4 字节随机值
        if (ERR_OK != _pgsql_md5_auth(pg, ev, breader)) {
            BIT_SET(*status, PROT_ERROR);
        }
        break;
    case 0x0a: { // AuthenticationSASL：SASL 认证，选择方法并发送 client-first-message
        const char *mod = _pgsql_get_authmod(pg, breader);
        if (NULL == mod) {
            BIT_SET(*status, PROT_ERROR);
            LOG_WARN("unsupported verification methods.");
            break;
        }
        if (ERR_OK != _pgsql_scram_client_first(pg, ev, mod)) {
            BIT_SET(*status, PROT_ERROR);
        }
        break;
    }
    case 0x0b: // AuthenticationSASLContinue：收到 server-first-message，发送 client-final-message
        if (ERR_OK != _pgsql_scram_client_final(pg, ev, breader)) {
            BIT_SET(*status, PROT_ERROR);
        }
        break;
    case 0x0c: // AuthenticationSASLFinal：验证 server-final-message 签名
        // 同 0x0b：要求 pg->scram 已被 0x0a 初始化，否则乱序消息触发 NULL 解引用
        if (NULL == pg->scram) {
            BIT_SET(*status, PROT_ERROR);
            LOG_WARN("received SASLFinal without prior SASL initialization.");
            break;
        }
        if (ERR_OK != scram_check_final_message(pg->scram,
            breader->data + breader->offset, breader->size - breader->offset)) {
            BIT_SET(*status, PROT_ERROR);
        }
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        LOG_WARN("unsupported verification methods.");
        break;
    }
}
// 处理认证阶段收到的服务端消息（R/S/K/Z/E）
static void _pgsql_auth_response(pgsql_ctx *pg, ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    int32_t lens;
    char *pack = _pgsql_payload(buf, &lens, status);
    if (NULL == pack) {
        return;
    }
    binary_ctx breader;
    binary_init(&breader, pack, lens + 1, 0); // +1 为类型码字节
    binary_get_skip(&breader, 5);             // 跳过类型码(1) + 长度(4)
    switch (pack[0]) {
    case 'E': { // ErrorResponse：认证失败，推送错误消息
        char *err = _pgpack_error_notice(&breader);
        _hs_push(pg->sk.fd, pg->sk.skid, 1, ud, ERR_FAILED, err, strlen(err));
        BIT_SET(*status, PROT_ERROR);
        break;
    }
    case 'R': // Authentication：认证请求，分派处理
        _pgsql_auth_process(pg, ev, &breader, status);
        break;
    case 'S': // ParameterStatus：运行时参数状态报告，忽略
        break;
    case 'K': // BackendKeyData：记录后端进程 ID 和取消密钥
        pg->pid = (int32_t)binary_get_integer(&breader, 4, 0);
        pg->key = (uint32_t)binary_get_integer(&breader, 4, 0);
        break;
    case 'Z': // ReadyForQuery：认证完成，服务端就绪，推送成功通知
        pg->readyforquery = binary_get_int8(&breader);
        ud->status = COMMAND;
        _hs_push(pg->sk.fd, pg->sk.skid, 1, ud, ERR_OK, NULL, 0);
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        break;
    }
    FREE(pack);
}
// 处理命令阶段收到的服务端消息，返回在 ReadyForQuery 时累积完成的 pgpack_ctx
static pgpack_ctx *_pgsql_command_response(pgsql_ctx *pg, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    int32_t lens;
    char *payload = _pgsql_payload(buf, &lens, status);
    if (NULL == payload) {
        return NULL;
    }
    binary_ctx breader;
    binary_init(&breader, payload, lens + 1, 0); // +1 为类型码字节
    return _pgpack_parser(pg, &breader, ud, status);
}
void *pgsql_unpack(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    pgsql_ctx *pg = (pgsql_ctx *)ud->context;
    pgpack_ctx *pack = NULL;
    switch (ud->status) {
    case INIT:    // 等待 SSL 协商响应
        _pgsql_ssl_response(pg, ev, buf, ud, status);
        break;
    case AUTH:    // 认证阶段消息处理
        _pgsql_auth_response(pg, ev, buf, ud, status);
        break;
    case COMMAND: // 命令阶段消息处理
        pack = _pgsql_command_response(pg, buf, ud, status);
        break;
    default:
        break;
    }
    return pack;
}
int32_t pgsql_init(pgsql_ctx *pg, const char *ip, uint16_t port, struct evssl_ctx *evssl,
    const char *user, const char *password, const char *database) {
    if (strlen(ip) > sizeof(pg->ip) - 1
        || strlen(user) > sizeof(pg->user) - 1
        || strlen(password) > sizeof(pg->password) - 1
        || (NULL != database && strlen(database) > sizeof(pg->database) - 1)) {
        return ERR_FAILED;
    }
    ZERO(pg, sizeof(pgsql_ctx));
    safe_fill_str(pg->ip, sizeof(pg->ip), ip);
    safe_fill_str(pg->user, sizeof(pg->user), user);
    safe_fill_str(pg->password, sizeof(pg->password), password);
    safe_fill_str(pg->database, sizeof(pg->database), NULL != database ? database : "");
    pg->port = 0 == port ? 5432 : port;
    pg->sk.fd = INVALID_SOCK;
    pg->evssl = evssl;
    return ERR_OK;
}
void pgsql_set_userpwd(pgsql_ctx *pg, const char *user, const char *password) {
    if (strlen(user) > sizeof(pg->user) - 1
        || strlen(password) > sizeof(pg->password) - 1) {
        LOG_ERROR("%s", "user or password too long.");
        return;
    }
    secure_zero(pg->user, sizeof(pg->user));
    secure_zero(pg->password, sizeof(pg->password));
    safe_fill_str(pg->user, sizeof(pg->user), user);
    safe_fill_str(pg->password, sizeof(pg->password), password);
}
void pgsql_set_db(pgsql_ctx *pg, const char *database) {
    if (strlen(database) > sizeof(pg->database) - 1) {
        LOG_ERROR("%s", "database name too long.");
        return;
    }
    safe_fill_str(pg->database, sizeof(pg->database), database);
}
const char *pgsql_get_db(pgsql_ctx *pg) {
    return pg->database;
}
int32_t pgsql_affected_rows(pgpack_ctx *pgpack) {
    size_t lens = strlen(pgpack->complete);
    if (0 == lens) {
        return 0;
    }
    // 从命令完成标签末尾反向找最后一个空格，取其后的数字字符串
    int32_t space = 1;
    for (int32_t i = (int32_t)lens - 1; i >= 0; i--) {
        if (space) {
            if (' ' != pgpack->complete[i]) {
                space = 0;
            }
            continue;
        }
        if (' ' == pgpack->complete[i]) {
            char *rows = pgpack->complete + i + 1;
            return (int32_t)strtol(rows, NULL, 10);
        }
    }
    return 0;
}
