#include "protocol/pgsql/pgsql.h"
#include "protocol/pgsql/pgsql_parse.h"
#include "crypt/scram.h"
#include "srey/task.h"
#include "utils/utils.h"

//https://www.postgresql.org/docs/18/protocol-flow.html

// 连接状态枚举
typedef enum parse_status {
    INIT = 0,   // 初始状态，等待 SSL 协商响应
    AUTH,       // 认证阶段，等待认证消息
    COMMAND     // 命令阶段，正常收发命令
}parse_status;

static const char *_pgsql_scram_mod[] = { "SCRAM-SHA-256" }; // 支持的 SASL 认证方法列表
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
    pg->fd = INVALID_SOCK;
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
    binary_set_string(&bwriter, "user", 0);
    binary_set_string(&bwriter, pg->user, 0);
    binary_set_string(&bwriter, "database", 0);
    binary_set_string(&bwriter, pg->database, 0);
    binary_set_string(&bwriter, "application_name", 0);
    binary_set_string(&bwriter, "srey", 0);
    binary_set_int8(&bwriter, 0);                           // 参数列表结束标志
    size_t size = bwriter.offset;
    binary_offset(&bwriter, 0);
    binary_set_integer(&bwriter, size, 4, 0);               // 回填消息总长度
    ud->status = AUTH;
    return ev_send(ev, pg->fd, pg->skid, bwriter.data, size, 0);
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
        // 服务端支持 SSL，升级连接
        if (NULL != pg->evssl) {
            if (ERR_OK != ev_ssl(ev, pg->fd, pg->skid, 1, pg->evssl)) {
                BIT_SET(*status, PROT_ERROR);
            }
        } else {
            BIT_SET(*status, PROT_ERROR);
            LOG_WARN("ssl not set.");
        }
        break;
    case 'N':
        // 服务端不支持 SSL，直接发送 Startup 消息
        if (ERR_OK != _pgsql_startup(ev, ud)) {
            BIT_SET(*status, PROT_ERROR);
        }
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        break;
    }
}
// SSL 握手完成后发送 Startup 消息
int32_t _pgsql_ssl_exchanged(ev_ctx *ev, ud_cxt *ud) {
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
    int32_t total = (*lens) + 1; // 消息总长度 = 类型码(1) + 消息体(lens)
    if ((size_t)total > blens) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    char *pack;
    MALLOC(pack, total);
    ASSERTAB(total == (int32_t)buffer_remove(buf, pack, total), "copy buffer failed.");
    return pack;
}
// 从 SASL 方法列表中查找本端支持的认证方法（当前仅支持 SCRAM-SHA-256）
static char *_pgsql_get_authmod(binary_ctx *breader) {
    char *mod;
    size_t i;
    size_t remain = breader->size - breader->offset;
    size_t n = ARRAY_SIZE(_pgsql_scram_mod);
    while (remain > 1) {
        mod = binary_get_string(breader, 0);
        for (i = 0; i < n; i++) {
            if (NULL != _pgsql_scram_mod[i]
                && 0 == strcmp(mod, _pgsql_scram_mod[i])) {
                return mod;
            }
        }
        remain -= (strlen(mod) + 1);
    }
    return NULL;
}
// 明文密码认证（AuthenticationCleartextPassword / GSSResponse）
static int32_t _pgsql_password_auth(pgsql_ctx *pg, ev_ctx *ev) {
    binary_ctx bwriter;
    pgsql_pack_start(&bwriter, 'p');
    binary_set_string(&bwriter, pg->password, 0);
    pgsql_pack_end(&bwriter);
    return ev_send(ev, pg->fd, pg->skid, bwriter.data, bwriter.offset, 0);
}
// SCRAM-SHA-256 第一步：发送 client-first-message
static int32_t _pgsql_scram_client_first(pgsql_ctx *pg, ev_ctx *ev, const char *mod) {
    pg->scram = scram_init(mod, 1);
    if (NULL == pg->scram) {
        return ERR_FAILED;
    }
    char *first_message = scram_first_message(pg->scram);
    binary_ctx bwriter;
    pgsql_pack_start(&bwriter, 'p');
    binary_set_string(&bwriter, mod, 0);                    // 所选 SASL 机制名称
    size_t fmlens = strlen(first_message);
    binary_set_integer(&bwriter, fmlens, 4, 0);             // client-first-message 长度
    binary_set_string(&bwriter, first_message, fmlens);
    FREE(first_message);
    pgsql_pack_end(&bwriter);
    return ev_send(ev, pg->fd, pg->skid, bwriter.data, bwriter.offset, 0);
}
// SCRAM-SHA-256 第二步：解析 server-first-message 并发送 client-final-message
static int32_t _pgsql_scram_client_final(pgsql_ctx *pg, ev_ctx *ev, binary_ctx *breader) {
    if (ERR_OK != scram_parse_first_message(pg->scram,
        breader->data + breader->offset, breader->size - breader->offset)) {
        return ERR_FAILED;
    }
    scram_set_pwd(pg->scram, pg->password);
    char *final_message = scram_final_message(pg->scram);
    if (NULL == final_message) {
        return ERR_FAILED;
    }
    binary_ctx bwriter;
    pgsql_pack_start(&bwriter, 'p');
    binary_set_string(&bwriter, final_message, strlen(final_message));
    FREE(final_message);
    pgsql_pack_end(&bwriter);
    return ev_send(ev, pg->fd, pg->skid, bwriter.data, bwriter.offset, 0);
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
        if (ERR_OK != _pgsql_password_auth(pg, ev)) {
            BIT_SET(*status, PROT_ERROR);
        }
        break;
    case 0x0a: { // AuthenticationSASL：SASL 认证，选择方法并发送 client-first-message
        const char *mod = _pgsql_get_authmod(breader);
        if (NULL == mod) {
            BIT_SET(*status, PROT_ERROR);
            LOG_WARN("unsupported verification methods.");
            break;
        }
        if (ERR_OK != _pgsql_scram_client_first(pg, ev, mod)) {
            BIT_SET(*status, PROT_ERROR);
        }
        break;
    case 0x0b: // AuthenticationSASLContinue：收到 server-first-message，发送 client-final-message
        if (ERR_OK != _pgsql_scram_client_final(pg, ev, breader)) {
            BIT_SET(*status, PROT_ERROR);
        }
        break;
    case 0x0c: // AuthenticationSASLFinal：验证 server-final-message 签名
        if (ERR_OK != scram_check_final_message(pg->scram,
            breader->data + breader->offset, breader->size - breader->offset)) {
            BIT_SET(*status, PROT_ERROR);
        }
        break;
    }
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
        _hs_push(pg->fd, pg->skid, 1, ud, ERR_FAILED, err, strlen(err));
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
        _hs_push(pg->fd, pg->skid, 1, ud, ERR_OK, NULL, 0);
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
    strcpy(pg->ip, ip);
    strcpy(pg->user, user);
    strcpy(pg->password, password);
    strcpy(pg->database, NULL != database ? database : "");
    pg->port = 0 == port ? 5432 : port;
    pg->fd = INVALID_SOCK;
    pg->evssl = evssl;
    return ERR_OK;
}
int32_t pgsql_try_connect(task_ctx *task, pgsql_ctx *pg) {
    pg->task = task;
    return task_connect(task, PACK_PGSQL, NULL, pg->ip, pg->port, NETEV_AUTHSSL, pg, &pg->fd, &pg->skid);
}
void pgsql_set_userpwd(pgsql_ctx *pg, const char *user, const char *password) {
    if (strlen(user) > sizeof(pg->user) - 1
        || strlen(password) > sizeof(pg->password) - 1) {
        LOG_ERROR("%s", "user or password too long.");
        return;
    }
    strcpy(pg->user, user);
    strcpy(pg->password, password);
}
void pgsql_set_db(pgsql_ctx *pg, const char *database) {
    if (strlen(database) > sizeof(pg->database) - 1) {
        LOG_ERROR("%s", "database name too long.");
        return;
    }
    strcpy(pg->database, database);
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
