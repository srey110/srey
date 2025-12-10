#include "protocol/pgsql/pgsql.h"
#include "srey/task.h"
#include "utils/binary.h"
#include "utils/utils.h"
#include "crypt/hmac.h"

//https://www.postgresql.org/docs/18/protocol-flow.html
typedef enum pgsql_client_status {
    LINKING = 0x01,
    AUTHED = 0x02
}pgsql_client_status;
typedef enum parse_status {
    INIT = 0,
    AUTH,
    COMMAND
}parse_status;
typedef enum pgsql_scram {
    SCRAM_SHA_256 = 0x00
}pgsql_scram;

static const char *_pgsql_authmod[] = {"SCRAM-SHA-256"};//下标与pgsql_scram对应
static _handshaked_push _hs_push;

void _pgsql_init(void *hspush) {
    _hs_push = hspush;
}
void _pgsql_pkfree(void *pack) {
    if (NULL == pack) {
        return;
    }
    pgpack_ctx *pgpack = pack;
    if (NULL != pgpack->_free_pgpack) {
        pgpack->_free_pgpack(pgpack->pack);
    }
    FREE(pgpack->pack);
    FREE(pgpack->payload);
    FREE(pgpack);
}
void _pgsql_udfree(ud_cxt *ud) {
    if (NULL == ud->extra) {
        return;
    }
    pgsql_ctx *pg = (pgsql_ctx *)ud->extra;
    _pgsql_pkfree(pg->pack);
    pg->pack = NULL;
    scram_free(pg->scram);
    pg->scram = NULL;
    pg->status = 0;
    ud->extra = NULL;
}
void _pgsql_closed(ud_cxt *ud) {
    _pgsql_udfree(ud);
}
//请求是否ssl加密
int32_t _pgsql_on_connected(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud, int32_t err) {
    if (ERR_OK != err) {
        pgsql_ctx *pg = (pgsql_ctx *)ud->extra;
        pg->status = 0;
        return err;
    }
    char buf[8];
    pack_integer(buf, 8, 4, 0);
    pack_integer(buf + 4, 80877103, 4, 0);
    ev_send(ev, fd, skid, buf, sizeof(buf), 1);
    return ERR_OK;
}
//Startup 消息
static void _pgsql_startup(ev_ctx *ev, ud_cxt *ud) {
    pgsql_ctx *pg = (pgsql_ctx *)ud->extra;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_skip(&bwriter, 4);
    binary_set_integer(&bwriter, 3, 2, 0);//major version
    binary_set_integer(&bwriter, 0, 2, 0);//minor version
    binary_set_string(&bwriter, "user", 0);
    binary_set_string(&bwriter, pg->user, 0);
    binary_set_string(&bwriter, "database", 0);
    binary_set_string(&bwriter, pg->database, 0);
    binary_set_string(&bwriter, "application_name", 0);
    binary_set_string(&bwriter, "srey", 0);
    binary_set_int8(&bwriter, 0);
    size_t size = bwriter.offset;
    binary_offset(&bwriter, 0);
    binary_set_integer(&bwriter, size, 4, 0);
    ud->status = AUTH;
    ev_send(ev, pg->fd, pg->skid, bwriter.data, size, 0);
}
//请求是否ssl加密 返回
static void _pgsql_ssl_response(pgsql_ctx *pg, ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    if (1 > buffer_size(buf)) {
        BIT_SET(*status, PROT_MOREDATA);
        return;
    }
    char ssl[1];
    ASSERTAB(sizeof(ssl) == buffer_remove(buf, ssl, sizeof(ssl)), "copy buffer failed.");
    switch (ssl[0]) {
    case 'S':
        if (NULL != pg->evssl) {
            ev_ssl(ev, pg->fd, pg->skid, 1, pg->evssl);
        } else {
            BIT_SET(*status, PROT_ERROR);
            LOG_WARN("ssl not set.");
        }
        break;
    case 'N':
        _pgsql_startup(ev, ud);//发送 Startup消息
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        break;
    }
}
//ssl握手完成发送 Startup消息
int32_t _pgsql_ssl_exchanged(ev_ctx *ev, ud_cxt *ud) {
    _pgsql_startup(ev, ud);
    return ERR_OK;
}
//获取一个完整的数据包
static char *_pgsql_payload(buffer_ctx *buf, int32_t *lens, int32_t *status) {
    size_t blens = buffer_size(buf);
    if (5 > blens) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    ASSERTAB(sizeof(*lens) == buffer_copyout(buf, 1, lens, sizeof(*lens)), "copy buffer failed.");
    *lens = (int32_t)unpack_integer((const char*)lens, 4, 0, 0);
    int32_t total = (*lens) + 1;
    if (total > blens) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    char *pack;
    MALLOC(pack, total);
    ASSERTAB(total == buffer_remove(buf, pack, total), "copy buffer failed.");
    return pack;
}
//ErrorResponse 'E' 解析
static int8_t _pgpack_err(pgsql_ctx *pg, binary_ctx *breader) {
    int8_t code = binary_get_int8(breader);
    if (ERR_OK != code) {
        char *tmp;
        size_t off = 0;
        size_t emlens, elens;
        size_t remain = breader->size - breader->offset;
        while (remain > 1) {
            tmp = binary_get_string(breader, 0);
            elens = strlen(tmp);
            emlens = sizeof(pg->error_msg) - off - 1;
            if (elens + 1 < emlens) {
                memcpy(pg->error_msg + off, tmp, elens);
                off += elens;
                memcpy(pg->error_msg + off, FLAG_CRLF, CRLF_SIZE);
                off += CRLF_SIZE;
                remain -= (elens + 1);
            } else {
                memcpy(pg->error_msg + off, tmp, emlens);
                off += emlens;
                break;
            }
        }
        if (off > CRLF_SIZE) {
            pg->error_msg[off - CRLF_SIZE] = '\0';
        } else {
            pg->error_msg[off] = '\0';
        }
    } else {
        pg->error_msg[0] = '\0';
    }
    return code;
}
//取得支持的认证方法
static char *_pgsql_get_authmod(pgsql_ctx *pg, binary_ctx *breader) {
    char *mod;
    int32_t i;
    size_t remain = breader->size - breader->offset;
    size_t n = ARRAY_SIZE(_pgsql_authmod);
    while (remain > 1) {
        mod = binary_get_string(breader, 0);
        for (i = 0; i < n; i++) {
            if (NULL != _pgsql_authmod[i]
                && 0 == STRCMP(mod, _pgsql_authmod[i])) {
                pg->scrammod = i;
                return mod;
            }
        }
        remain -= (strlen(mod) + 1);
    }
    return NULL;
}
//password方式认证 GSSResponse 
static void _pgsql_password_auth(pgsql_ctx *pg, ev_ctx *ev, ud_cxt *ud) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, 'p');
    binary_set_skip(&bwriter, 4);
    binary_set_string(&bwriter, pg->password, 0);
    size_t size = bwriter.offset;
    binary_offset(&bwriter, 1);
    binary_set_integer(&bwriter, size - 1, 4, 0);
    ev_send(ev, pg->fd, pg->skid, bwriter.data, size, 0);
}
//scram-sha-256 第一步
static void _pgsql_scram_client_first(pgsql_ctx *pg, ev_ctx *ev, ud_cxt *ud, const char *mod) {
    pg->scram = scram_init(DG_SHA256);
    char *first_message = scram_client_first_message(pg->scram, NULL);
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, 'p');
    binary_set_skip(&bwriter, 4);
    binary_set_string(&bwriter, mod, 0);
    size_t fmlens = strlen(first_message);
    binary_set_integer(&bwriter, fmlens, 4, 0);
    binary_set_string(&bwriter, first_message, fmlens);
    FREE(first_message);
    size_t size = bwriter.offset;
    binary_offset(&bwriter, 1);
    binary_set_integer(&bwriter, size - 1, 4, 0);
    ev_send(ev, pg->fd, pg->skid, bwriter.data, size, 0);
}
//scram-sha-256 第二步
static void _pgsql_scram_client_final(pgsql_ctx *pg, ev_ctx *ev, binary_ctx *breader, ud_cxt *ud, int32_t *status) {
   if (ERR_OK != scram_read_server_first_message(pg->scram, breader)) {
       BIT_SET(*status, PROT_ERROR);
       return;
   }
   char *final_message = scram_client_final_message(pg->scram, pg->password);
   binary_ctx bwriter;
   binary_init(&bwriter, NULL, 0, 0);
   binary_set_int8(&bwriter, 'p');
   binary_set_skip(&bwriter, 4);
   binary_set_string(&bwriter, final_message, strlen(final_message));
   FREE(final_message);
   size_t size = bwriter.offset;
   binary_offset(&bwriter, 1);
   binary_set_integer(&bwriter, size - 1, 4, 0);
   ev_send(ev, pg->fd, pg->skid, bwriter.data, size, 0);
}
static void _pgsql_auth_process(pgsql_ctx *pg, ev_ctx *ev, binary_ctx *breader, ud_cxt *ud, int32_t *status) {
    int32_t code = (int32_t)binary_get_integer(breader, 4, 0);
    switch (code) {
    case 0x00://认证成功 AuthenticationOk 
        scram_free(pg->scram);
        pg->scram = NULL;
        ud->status = COMMAND;
        BIT_SET(pg->status, AUTHED);
        _hs_push(pg->fd, pg->skid, 1, ud, ERR_OK, NULL, 0);
        break;
    case 0x03://明文密码 AuthenticationCleartextPassword 
        _pgsql_password_auth(pg, ev, ud);
        break;
    case 0x0a: {//SASL身份验证 AuthenticationSASL
        const char *mod = _pgsql_get_authmod(pg, breader);
        if (NULL == mod) {
            BIT_SET(*status, PROT_ERROR);
            LOG_WARN("unsupported verification methods.");
            break;
        }
        ASSERTAB(NULL == pg->scram, "scram error.");
        _pgsql_scram_client_first(pg, ev, ud, mod);
        break;
    case 0x0b://AuthenticationSASLContinue
        _pgsql_scram_client_final(pg, ev, breader, ud, status);
        break;
    case 0x0c://AuthenticationSASLFinal 
        if (ERR_OK != scram_server_final(pg->scram, breader)) {
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
static void _pgsql_auth_response(pgsql_ctx *pg, ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    int32_t lens;
    char *pack = _pgsql_payload(buf, &lens, status);
    if (NULL == pack) {
        return;
    }
    binary_ctx breader;
    binary_init(&breader, pack, lens + 1, 0);//1 操作码
    binary_get_skip(&breader, 5);
    switch (pack[0]) {
    case 'E': {
        int8_t code = _pgpack_err(pg, &breader);
        LOG_WARN("error code %d, message: %s", code, pg->error_msg);
        _hs_push(pg->fd, pg->skid, 1, ud, ERR_FAILED, NULL, 0);
        BIT_SET(*status, PROT_ERROR);
        break;
    }
    case 'R':
        _pgsql_auth_process(pg, ev, &breader, ud, status);
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        break;
    }
    FREE(pack);
}
static pgpack_ctx *_pgsql_command_response(pgsql_ctx *pg, ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    int32_t lens;
    char *payload = _pgsql_payload(buf, &lens, status);
    if (NULL == payload) {
        return NULL;
    }
    pgpack_ctx *pack = NULL;
    binary_ctx breader;
    binary_init(&breader, payload, lens + 1, 0);//1 操作码
    binary_get_skip(&breader, 5);
    switch (payload[0]) {
    case 'E':
        break;
    case 'S'://ParameterStatus 运行时参数状态报告
        FREE(payload);
        break;
    case 'K'://BackendKeyData 取消键数据
        pg->pid = (int32_t)binary_get_integer(&breader, 4, 0);
        pg->key = (uint32_t)binary_get_integer(&breader, 4, 0);
        FREE(payload);
        break;
    case 'Z'://ReadyForQuery
        pg->readyforquery = binary_get_int8(&breader);
        FREE(payload);
        break;
    default:
        LOG_WARN("unknown opcode: %d", payload[0]);
        FREE(payload);
        break;
    }
    return pack;
}
void *pgsql_unpack(ev_ctx *ev, buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    pgsql_ctx *pg = (pgsql_ctx *)ud->extra;
    pgpack_ctx *pack = NULL;
    switch (ud->status) {
    case INIT:
        _pgsql_ssl_response(pg, ev, buf, ud, status);
        break;
    case AUTH:
        _pgsql_auth_response(pg, ev, buf, ud, status);
        break;
    case COMMAND:
        pack = _pgsql_command_response(pg, ev, buf, ud, status);
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
    strcpy(pg->database, database);
    pg->port = 0 == port ? 5432 : port;
    pg->evssl = evssl;
    return ERR_OK;
}
int32_t pgsql_try_connect(task_ctx *task, pgsql_ctx *pg) {
    if (0 != pg->status) {
        return ERR_FAILED;
    }
    pg->task = task;
    BIT_SET(pg->status, LINKING);
    if (ERR_OK != task_connect(task, PACK_PGSQL, NULL, pg->ip, pg->port, NETEV_AUTHSSL, pg, &pg->fd, &pg->skid)) {
        BIT_REMOVE(pg->status, LINKING);
        return ERR_FAILED;
    }
    return ERR_OK;
}
void *pgsql_pack_quit(pgsql_ctx *pg, size_t *size) {
    if (!BIT_CHECK(pg->status, AUTHED)) {
        return NULL;
    }
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, 'X');//Terminate
    binary_set_integer(&bwriter, 4, 4, 0);
    *size = bwriter.offset;
    return bwriter.data;
}
