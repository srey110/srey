#include "srey/coro_utils.h"
#include "srey/coro.h"
#include "srey/task.h"
#include "protocol/urlparse.h"
#include "protocol/dns.h"
#include "protocol/websock.h"
#include "protocol/http.h"
#include "protocol/redis.h"
#include "protocol/mysql/mysql_parse.h"
#include "protocol/mysql/mysql_pack.h"
#include "protocol/mongo/bson.h"
#include "utils/buffer.h"

dns_ip *dns_lookup(task_ctx *task, const char *domain, int32_t ipv6, size_t *cnt) {
    int32_t rtn;
    SOCKET fd;
    uint64_t skid;
    const char *dnsip = dns_get_ip();
    if (ERR_OK == is_ipv6(dnsip)) {
        rtn = task_udp(task, "::", 0, &fd, &skid);
    } else {
        rtn = task_udp(task, "0.0.0.0", 0, &fd, &skid);
    }
    if (ERR_OK != rtn) {
        LOG_WARN("init udp error.");
        return NULL;
    }
    char buf[ONEK] = { 0 };
    size_t lens = dns_request_pack(buf, domain, ipv6);
    void *resp = coro_sendto(task, fd, skid, dnsip, 53, buf, lens, &lens, 1);
    ev_close(&task->loader->netev, fd, skid);
    if (NULL == resp) {
        return NULL;
    }
    return dns_parse_pack(resp, lens, cnt);
}
SOCKET wbsock_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ws, const char *secprot, uint64_t *skid, int32_t netev) {
    url_ctx url;
    url_parse(&url, (char *)ws, strlen(ws));
    int32_t isws = buf_icompare(&url.scheme, "ws", strlen("ws"));
    int32_t iswss = buf_icompare(&url.scheme, "wss", strlen("wss"));
    if (!isws
        && !iswss) {
        return INVALID_SOCK;
    }
    if (iswss
        && NULL == evssl) {
        return INVALID_SOCK;
    }
    if (0 == url.host.lens) {
        return INVALID_SOCK;
    }
    char *host;
    char ip[IP_LENS] = { 0 };
    CALLOC(host, 1, url.host.lens + 1);
    memcpy(host, url.host.data, url.host.lens);
    if (ERR_OK != is_ipaddr(host)) {
        size_t nips;
        dns_ip *ips = dns_lookup(task, host, 0, &nips);
        if (NULL == ips) {
            FREE(host);
            return INVALID_SOCK;
        }
        if (0 == nips) {
            FREE(host);
            FREE(ips);
            return INVALID_SOCK;
        }
        memcpy(ip, ips[0].ip, strlen(ips[0].ip));
        FREE(ips);
    } else {
        memcpy(ip, host, strlen(host));
    }
    uint16_t port;
    if (url.port.lens > 0) {
        port = (uint16_t)strtol(url.port.data, NULL, 10);
    } else {
        port = NULL == evssl ? 80 : 443;
    }
    SOCKET fd;
    if (ERR_OK != coro_connect(task, PACK_WEBSOCK, evssl, ip, port, netev, NULL, &fd, skid)) {
        FREE(host);
        return INVALID_SOCK;
    }
    char *reqpack = websock_pack_handshake(host, secprot);
    FREE(host);
    if (ERR_OK != ev_send(&task->loader->netev, fd, *skid, reqpack, strlen(reqpack), 0)) {
        return INVALID_SOCK;
    }
    int32_t err;
    size_t size;
    char *hsdata = coro_handshaked(task, fd, *skid, &err, &size);
    if (ERR_OK != err) {
        return INVALID_SOCK;
    }
    if (!EMPTYSTR(secprot)) {
        size_t seclens = strlen(secprot);
        if (NULL != secprot
            && 0 != seclens) {
            if (seclens != size || 0 != memcmp(secprot, hsdata, seclens)) {
                ev_close(&task->loader->netev, fd, *skid);
                return INVALID_SOCK;
            }
        }
    }
    return fd;
}
SOCKET redis_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ip, uint16_t port, const char *key, uint64_t *skid, int32_t netev) {
    SOCKET fd;
    if (ERR_OK != coro_connect(task, PACK_REDIS, evssl, ip, port, netev, NULL, &fd, skid)) {
        return INVALID_SOCK;
    }
    if (!EMPTYSTR(key)) {
        size_t size;
        char *auth = redis_pack(&size, "AUTH %s", key);
        redis_pack_ctx *rtn = coro_send(task, fd, *skid, auth, size, NULL, 0);
        if (NULL == rtn
            || RESP_STRING != rtn->prot
            || 2 != rtn->len
            || 0 != _memicmp(rtn->data, "ok", (size_t)rtn->len)) {
            ev_close(&task->loader->netev, fd, *skid);
            return INVALID_SOCK;
        }
    }
    return fd;
}
int32_t mysql_connect(task_ctx *task, mysql_ctx *mysql) {
    if (ERR_OK != mysql_try_connect(task, mysql)) {
        return ERR_FAILED;
    }
    int32_t err;
    coro_handshaked(task, mysql->client.fd, mysql->client.skid, &err, NULL);
    if (ERR_OK == err) {
        coro_sync(task, mysql->client.fd, mysql->client.skid);
    }
    return err;
}
int32_t mysql_selectdb(mysql_ctx *mysql, const char *database) {
    size_t size;
    void *selectdb = mysql_pack_selectdb(mysql, database, &size);
    mpack_ctx *mpack = coro_send(mysql->task, mysql->client.fd, mysql->client.skid, selectdb, size, NULL, 0);
    if (NULL == mpack) {
        return ERR_FAILED;
    }
    return MPACK_OK == mpack->pack_type ? ERR_OK : ERR_FAILED;
}
// 向 MySQL 服务器发送 ping 包并等待响应，失败返回 ERR_FAILED
static int32_t _mysql_ping(mysql_ctx *mysql) {
    size_t size;
    void *ping = mysql_pack_ping(mysql, &size);
    mpack_ctx *mpack = coro_send(mysql->task, mysql->client.fd, mysql->client.skid, ping, size, NULL, 0);
    if (NULL == mpack) {
        return ERR_FAILED;
    }
    return MPACK_OK == mpack->pack_type ? ERR_OK : ERR_FAILED;
}
int32_t mysql_ping(mysql_ctx *mysql) {
    if (ERR_OK != _mysql_ping(mysql)) {
        return mysql_connect(mysql->task, mysql);
    }
    return ERR_OK;
}
mpack_ctx *mysql_query(mysql_ctx *mysql, const char *sql, mysql_bind_ctx *mbind) {
    size_t size;
    void *query = mysql_pack_query(mysql, sql, mbind, &size);
    return coro_send(mysql->task, mysql->client.fd, mysql->client.skid, query, size, NULL, 0);
}
mysql_stmt_ctx *mysql_stmt_prepare(mysql_ctx *mysql, const char *sql) {
    size_t size;
    void *prepare = mysql_pack_stmt_prepare(mysql, sql, &size);
    mpack_ctx *mpack = coro_send(mysql->task, mysql->client.fd, mysql->client.skid, prepare, size, NULL, 0);
    return mysql_stmt_init(mpack);
}
mpack_ctx *mysql_stmt_execute(mysql_stmt_ctx *stmt, mysql_bind_ctx *mbind) {
    size_t size;
    void *exec = mysql_pack_stmt_execute(stmt, mbind, &size);
    return coro_send(stmt->mysql->task, stmt->mysql->client.fd, stmt->mysql->client.skid, exec, size, NULL, 0);
}
int32_t mysql_stmt_reset(mysql_stmt_ctx *stmt) {
    size_t size;
    void *resetpk = mysql_pack_stmt_reset(stmt, &size);
    mpack_ctx *mpack = coro_send(stmt->mysql->task, stmt->mysql->client.fd, stmt->mysql->client.skid, resetpk, size, NULL, 0);
    if (NULL == mpack) {
        return ERR_FAILED;
    }
    return MPACK_OK == mpack->pack_type ? ERR_OK : ERR_FAILED;
}
void mysql_quit(mysql_ctx *mysql) {
    size_t size;
    void *quit = mysql_pack_quit(mysql, &size);
    coro_send(mysql->task, mysql->client.fd, mysql->client.skid, quit, size, NULL, 0);
}
int32_t smtp_connect(task_ctx *task, smtp_ctx *smtp) {
    if (ERR_OK != smtp_try_connect(task, smtp)) {
        return ERR_FAILED;
    }
    int32_t err;
    char *msg = (char *)coro_handshaked(task, smtp->fd, smtp->skid, &err, NULL);
    if (ERR_OK != err) {
        if (NULL != msg) {
            LOG_WARN("%s", msg);
        }
    } else {
        coro_sync(task, smtp->fd, smtp->skid);
    }
    return err;
}
// 发送 SMTP QUIT 命令并等待响应（不关闭 socket）
static void _smtp_quit(smtp_ctx *smtp) {
    char *cmd = smtp_pack_quit();
    char *pack = coro_send(smtp->task, smtp->fd, smtp->skid, cmd, strlen(cmd), NULL, 0);
    if (NULL == pack) {
        return;
    }
    smtp_check_code(pack, "221");
}
void smtp_quit(smtp_ctx *smtp) {
    _smtp_quit(smtp);
    ev_close(&smtp->task->loader->netev, smtp->fd, smtp->skid);
}
// 发送 SMTP NOOP 命令检测连接是否存活，失败返回 ERR_FAILED
static int32_t _smtp_ping(smtp_ctx *smtp) {
    char *cmd = smtp_pack_ping();
    char *pack = coro_send(smtp->task, smtp->fd, smtp->skid, cmd, strlen(cmd), NULL, 0);
    if (NULL == pack) {
        return ERR_FAILED;
    }
    return smtp_check_ok(pack);
}
int32_t smtp_ping(smtp_ctx *smtp) {
    if (ERR_OK != _smtp_ping(smtp)) {
        return smtp_connect(smtp->task, smtp);
    }
    return ERR_OK;
}
// 执行 SMTP 邮件发送流程（MAIL FROM → RCPT TO → DATA → 正文）
static int32_t _smtp_send(smtp_ctx *smtp, mail_ctx *mail) {
    char *cmd = smtp_pack_from(mail->from.addr);
    char *pack = coro_send(smtp->task, smtp->fd, smtp->skid, cmd, strlen(cmd), NULL, 0);
    if (NULL == pack
        || ERR_OK != smtp_check_ok(pack)) {
        return ERR_FAILED;
    }
    uint32_t naddr = arr_mail_addr_size(&mail->addrs);
    for (uint32_t i = 0; i < naddr; i++) {
        cmd = smtp_pack_rcpt(arr_mail_addr_at(&mail->addrs, i)->addr);
        pack = coro_send(smtp->task, smtp->fd, smtp->skid, cmd, strlen(cmd), NULL, 0);
        if (NULL == pack
            || ERR_OK != smtp_check_ok(pack)) {
            return ERR_FAILED;
        }
    }
    cmd = smtp_pack_data();
    pack = coro_send(smtp->task, smtp->fd, smtp->skid, cmd, strlen(cmd), NULL, 0);
    if (NULL == pack) {
        return ERR_FAILED;
    }
    if (ERR_OK != smtp_check_code(pack, "354")) {
        return ERR_FAILED;
    }
    cmd = mail_pack(mail);
    pack = coro_send(smtp->task, smtp->fd, smtp->skid, cmd, strlen(cmd), NULL, 0);
    if (NULL == pack
        || ERR_OK != smtp_check_ok(pack)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 发送 SMTP RSET 命令重置会话状态（不关闭连接）
static int32_t _smtp_reset(smtp_ctx *smtp) {
    char *cmd = smtp_pack_reset();
    char *pack = coro_send(smtp->task, smtp->fd, smtp->skid, cmd, strlen(cmd), NULL, 0);
    if (NULL == pack) {
        return ERR_FAILED;
    }
    return smtp_check_ok(pack);
}
int32_t smtp_send(smtp_ctx *smtp, mail_ctx *mail) {
    int32_t rtn = _smtp_send(smtp, mail);
    _smtp_reset(smtp);
    return rtn;
}
int32_t pgsql_connect(task_ctx *task, pgsql_ctx *pg) {
    if (ERR_OK != pgsql_try_connect(task, pg)) {
        return ERR_FAILED;
    }
    int32_t code;
    char *err = coro_handshaked(task, pg->fd, pg->skid, &code, NULL);
    if (ERR_OK != code) {
        if (NULL != err) {
            LOG_WARN("%s", err);
        }
    } else {
        coro_sync(task, pg->fd, pg->skid);
    }
    return code;
}
void pgsql_quit(pgsql_ctx *pg) {
    size_t lens;
    void *quit = pgsql_pack_terminate(&lens);
    coro_send(pg->task, pg->fd, pg->skid, quit, lens, NULL, 0);
}
int32_t pgsql_selectdb(pgsql_ctx *pg, const char *database) {
    pgsql_quit(pg);
    pgsql_set_db(pg, database);
    return pgsql_connect(pg->task, pg);
}
int32_t pgsql_ping(pgsql_ctx *pg) {
    pgpack_ctx *pgpack = pgsql_query(pg, ";");
    if (NULL == pgpack) {
        return pgsql_connect(pg->task, pg);
    }
    return ERR_OK;
}
pgpack_ctx *pgsql_query(pgsql_ctx *pg, const char *sql) {
    size_t lens;
    void *query = pgsql_pack_query(sql, &lens);
    return coro_send(pg->task, pg->fd, pg->skid, query, lens, NULL, 0);
}
int32_t pgsql_stmt_prepare(pgsql_ctx *pg, const char *name, const char *sql, int16_t nparam, uint32_t *oids) {
    if (EMPTYSTR(sql)) {
        return ERR_FAILED;
    }
    size_t lens;
    void *parse = pgsql_pack_stmt_prepare(name, sql, nparam, oids, &lens);
    pgpack_ctx *pgpack = coro_send(pg->task, pg->fd, pg->skid, parse, lens, NULL, 0);
    if (NULL == pgpack) {
        return ERR_FAILED;
    }
    if (PGPACK_ERR == pgpack->type) {
        LOG_WARN("%s", (const char *)pgpack->pack);
        return ERR_FAILED;
    }
    return PGPACK_OK == pgpack->type ? ERR_OK : ERR_FAILED;
}
pgpack_ctx *pgsql_stmt_execute(pgsql_ctx *pg, const char *name, pgsql_bind_ctx *bind, pgpack_format resultformat) {
    size_t lens;
    void *exec = pgsql_pack_stmt_execute(name, bind, resultformat, &lens);
    return coro_send(pg->task, pg->fd, pg->skid, exec, lens, NULL, 0);
}
void pgsql_stmt_close(pgsql_ctx *pg, const char *name) {
    size_t lens;
    void *close = pgsql_pack_stmt_close(name, &lens);
    coro_send(pg->task, pg->fd, pg->skid, close, lens, NULL, 0);
}
int32_t mongo_connect(task_ctx *task, mongo_ctx *mongo) {
    mongo->task = task;
    mongo_set_error(mongo, NULL, 0);
    int32_t rtn = coro_connect(task, PACK_MONGO, mongo->evssl, mongo->ip, mongo->port, 0, mongo, &mongo->fd, &mongo->skid);
    if (ERR_OK != rtn) {
        mongo_set_error(mongo, "connect error.", 1);
    }
    return rtn;
}
// 执行 MongoDB SCRAM 认证流程（发送 client-first 消息并等待握手结果）
static int32_t _mongo_auth(mongo_ctx *mongo, const char *authmod) {
    size_t lens;
    void *client_first = mongo_pack_scram_client_first(mongo, authmod, &lens);
    if (NULL == client_first) {
        mongo_set_error(mongo, "create scram client first message error.", 1);
        return ERR_FAILED;
    }
    ev_ud_status(&mongo->task->loader->netev, mongo->fd, mongo->skid, mongo_status_auth());
    if (ERR_OK != ev_send(&mongo->task->loader->netev, mongo->fd, mongo->skid, client_first, lens, 0)) {
        mongo_set_error(mongo, "send authentication message error.", 1);
        return ERR_FAILED;
    }
    int32_t err;
    mgopack_ctx *mgpack = coro_handshaked(mongo->task, mongo->fd, mongo->skid, &err, NULL);
    if (ERR_OK != err) {
        if (NULL != mgpack) {
            mongo_set_error(mongo, bson_tostring2(mgpack->doc, mgpack->dlens), 0);
        } else {
            mongo_set_error(mongo, "authentication failed.", 1);
        }
    }
    return err;
}
int32_t mongo_auth(mongo_ctx *mongo, const char *authmod, const char *user, const char *pwd) {
    int32_t flags = mongo_clear_flag(mongo);
    mongo_set_error(mongo, NULL, 0);
    mongo_user_pwd(mongo, user, pwd);
    int32_t rtn = _mongo_auth(mongo, authmod);
    mongo_set_flag(mongo, flags);
    return rtn;
}
mgopack_ctx *mongo_hello(mongo_ctx *mongo, char *options) {
    int32_t flags = mongo_clear_flag(mongo);
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *hello = mongo_pack_hello(mongo, options, &lens);
    mongo_set_flag(mongo, flags);
    mgopack_ctx *mgpack = coro_send(mongo->task, mongo->fd, mongo->skid, hello, lens, NULL, 0);
    if (NULL == mgpack) {
        mongo_set_error(mongo, "send hello message error.", 1);
        return NULL;
    }
    if (ERR_FAILED == mongo_parse_check_error(mongo, mgpack)) {
        return NULL;
    }
    return mgpack;
}
int32_t mongo_ping(mongo_ctx *mongo) {
    int32_t flags = mongo_clear_flag(mongo);
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *ping = mongo_pack_ping(mongo, &lens);
    mongo_set_flag(mongo, flags);
    mgopack_ctx *mgpack = coro_send(mongo->task, mongo->fd, mongo->skid, ping, lens, NULL, 0);
    if (NULL == mgpack) {
        mongo_set_error(mongo, "send ping message error.", 1);
        return ERR_FAILED;
    }
    if (ERR_FAILED == mongo_parse_check_error(mongo, mgpack)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
// MongoDB 统一发送函数：设置了 MORETOCOME 标志时仅发送不等待响应，否则同步等待响应
static inline int32_t _mongo_send(mongo_ctx *mongo, const char *cmdname, void *pack, size_t lens, mgopack_ctx **mgopack) {
    if (mongo_check_flag(mongo, MORETOCOME)) {
        if (ERR_OK != ev_send(&mongo->task->loader->netev, mongo->fd, mongo->skid, pack, lens, 0)) {
            char err[64];
            SNPRINTF(err, sizeof(err), "send %s message error.", cmdname);
            mongo_set_error(mongo, err, 1);
            return ERR_FAILED;
        }
        return ERR_OK;
    }
    mgopack_ctx *rtnpack = coro_send(mongo->task, mongo->fd, mongo->skid, pack, lens, NULL, 0);
    if (NULL == rtnpack) {
        char err[64];
        SNPRINTF(err, sizeof(err), "send %s message error.", cmdname);
        mongo_set_error(mongo, err, 1);
        return ERR_FAILED;
    }
    SET_PTR(mgopack, rtnpack);
    return ERR_OK;
}
int32_t mongo_drop(mongo_ctx *mongo, char *options) {
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *drop = mongo_pack_drop(mongo, options, &lens);
    mgopack_ctx *mgpack = NULL;
    if (ERR_OK != _mongo_send(mongo, "drop", drop, lens, &mgpack)) {
        return ERR_FAILED;
    }
    if (NULL == mgpack) {
        return ERR_OK;
    }
    if (ERR_FAILED == mongo_parse_check_error(mongo, mgpack)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t mongo_insert(mongo_ctx *mongo, char *docs, size_t dlens, char *options) {
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *insert = mongo_pack_insert(mongo, docs, dlens, options, &lens);
    mgopack_ctx *mgpack = NULL;
    if (ERR_OK != _mongo_send(mongo, "insert", insert, lens, &mgpack)) {
        return ERR_FAILED;
    }
    if (NULL == mgpack) {
        return ERR_OK;
    }
    return mongo_parse_check_error(mongo, mgpack);
}
int32_t mongo_update(mongo_ctx *mongo, char *updates, size_t ulens, char *options) {
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *update = mongo_pack_update(mongo, updates, ulens, options, &lens);
    mgopack_ctx *mgpack = NULL;
    if (ERR_OK != _mongo_send(mongo, "update", update, lens, &mgpack)) {
        return ERR_FAILED;
    }
    if (NULL == mgpack) {
        return ERR_OK;
    }
    return mongo_parse_check_error(mongo, mgpack);
}
int32_t mongo_delete(mongo_ctx *mongo, char *deletes, size_t dlens, char *options) {
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *del = mongo_pack_delete(mongo, deletes, dlens, options, &lens);
    mgopack_ctx *mgpack = NULL;
    if (ERR_OK != _mongo_send(mongo, "delete", del, lens, &mgpack)) {
        return ERR_FAILED;
    }
    if (NULL == mgpack) {
        return ERR_OK;
    }
    return mongo_parse_check_error(mongo, mgpack);
}
mgopack_ctx *mongo_bulkwrite(mongo_ctx *mongo, char *ops, size_t olens, char *nsinfo, size_t nlens, char *options) {
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *bulkwrite = mongo_pack_bulkwrite(mongo, ops, olens, nsinfo, nlens, options, &lens);
    mgopack_ctx *mgpack = NULL;
    if (ERR_OK != _mongo_send(mongo, "bulkwrite", bulkwrite, lens, &mgpack)) {
        return NULL;
    }
    if (NULL == mgpack) {
        return NULL;
    }
    if (ERR_FAILED == mongo_parse_check_error(mongo, mgpack)) {
        return NULL;
    }
    return mgpack;
}
mgopack_ctx *mongo_find(mongo_ctx *mongo, char *filter, size_t flens, char *options) {
    int32_t flags = mongo_clear_flag(mongo);
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *find = mongo_pack_find(mongo, filter, flens, options, &lens);
    mongo_set_flag(mongo, flags);
    mgopack_ctx *mgpack = coro_send(mongo->task, mongo->fd, mongo->skid, find, lens, NULL, 0);
    if (NULL == mgpack) {
        mongo_set_error(mongo, "send find message error.", 1);
        return NULL;
    }
    if (ERR_FAILED == mongo_parse_check_error(mongo, mgpack)) {
        return NULL;
    }
    return mgpack;
}
mgopack_ctx *mongo_aggregate(mongo_ctx *mongo, char *pipeline, size_t pllens, char *options) {
    int32_t flags = mongo_clear_flag(mongo);
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *aggt = mongo_pack_aggregate(mongo, pipeline, pllens, options, &lens);
    mongo_set_flag(mongo, flags);
    mgopack_ctx *mgpack = coro_send(mongo->task, mongo->fd, mongo->skid, aggt, lens, NULL, 0);
    if (NULL == mgpack) {
        mongo_set_error(mongo, "send aggregate message error.", 1);
        return NULL;
    }
    if (ERR_FAILED == mongo_parse_check_error(mongo, mgpack)) {
        return NULL;
    }
    return mgpack;
}
mgopack_ctx *mongo_getmore(mongo_ctx *mongo, int64_t cursorid, char *options) {
    int32_t flags = mongo_clear_flag(mongo);
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *getmore = mongo_pack_getmore(mongo, cursorid, options, &lens);
    mongo_set_flag(mongo, flags);
    mgopack_ctx *mgpack = coro_send(mongo->task, mongo->fd, mongo->skid, getmore, lens, NULL, 0);
    if (NULL == mgpack) {
        mongo_set_error(mongo, "send getmore message error.", 1);
        return NULL;
    }
    if (ERR_FAILED == mongo_parse_check_error(mongo, mgpack)) {
        return NULL;
    }
    return mgpack;
}
mgopack_ctx *mongo_killcursors(mongo_ctx *mongo, char *cursorids, size_t cslens, char *options) {
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *killcursors = mongo_pack_killcursors(mongo, cursorids, cslens, options, &lens);
    mgopack_ctx *mgpack = NULL;
    if (ERR_OK != _mongo_send(mongo, "killcursors", killcursors, lens, &mgpack)) {
        return NULL;
    }
    if (NULL == mgpack) {
        return NULL;
    }
    if (ERR_FAILED == mongo_parse_check_error(mongo, mgpack)) {
        return NULL;
    }
    return mgpack;
}
mgopack_ctx *mongo_distinct(mongo_ctx *mongo, const char *key, char *query, size_t qlens, char *options) {
    int32_t flags = mongo_clear_flag(mongo);
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *distinct = mongo_pack_distinct(mongo, key, query, qlens, options, &lens);
    mongo_set_flag(mongo, flags);
    mgopack_ctx *mgpack = coro_send(mongo->task, mongo->fd, mongo->skid, distinct, lens, NULL, 0);
    if (NULL == mgpack) {
        mongo_set_error(mongo, "send distinct message error.", 1);
        return NULL;
    }
    if (ERR_FAILED == mongo_parse_check_error(mongo, mgpack)) {
        return NULL;
    }
    return mgpack;
}
mgopack_ctx *mongo_findandmodify(mongo_ctx *mongo, char *query, size_t qlens,
    int32_t remove, int32_t pipeline, char *update, size_t ulens, char *options) {
    int32_t flags = mongo_clear_flag(mongo);
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *findandmodify = mongo_pack_findandmodify(mongo, query, qlens, remove, pipeline, update, ulens, options, &lens);
    mongo_set_flag(mongo, flags);
    mgopack_ctx *mgpack = coro_send(mongo->task, mongo->fd, mongo->skid, findandmodify, lens, NULL, 0);
    if (NULL == mgpack) {
        mongo_set_error(mongo, "send findandmodify message error.", 1);
        return NULL;
    }
    if (ERR_FAILED == mongo_parse_check_error(mongo, mgpack)) {
        return NULL;
    }
    return mgpack;
}
int32_t mongo_count(mongo_ctx *mongo, char *query, size_t qlens, char *options) {
    int32_t flags = mongo_clear_flag(mongo);
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *count = mongo_pack_count(mongo, query, qlens, options, &lens);
    mongo_set_flag(mongo, flags);
    mgopack_ctx *mgpack = coro_send(mongo->task, mongo->fd, mongo->skid, count, lens, NULL, 0);
    if (NULL == mgpack) {
        mongo_set_error(mongo, "send count message error.", 1);
        return ERR_FAILED;
    }
    return mongo_parse_check_error(mongo, mgpack);
}
int32_t mongo_createindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options) {
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *createindexes = mongo_pack_createindexes(mongo, indexes, ilens, options, &lens);
    mgopack_ctx *mgpack = NULL;
    if (ERR_OK != _mongo_send(mongo, "createindexes", createindexes, lens, &mgpack)) {
        return ERR_FAILED;
    }
    if (NULL == mgpack) {
        return ERR_OK;
    }
    if (ERR_FAILED == mongo_parse_check_error(mongo, mgpack)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t mongo_dropindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options) {
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *dropindexes = mongo_pack_dropindexes(mongo, indexes, ilens, options, &lens);
    mgopack_ctx *mgpack = NULL;
    if (ERR_OK != _mongo_send(mongo, "dropindexes", dropindexes, lens, &mgpack)) {
        return ERR_FAILED;
    }
    if (NULL == mgpack) {
        return ERR_OK;
    }
    if (ERR_FAILED == mongo_parse_check_error(mongo, mgpack)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
mongo_session *mongo_startsession(mongo_ctx *mongo) {
    int32_t flags = mongo_clear_flag(mongo);
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *startsession = mongo_pack_startsession(mongo, &lens);
    mongo_set_flag(mongo, flags);
    mgopack_ctx *mgpack = coro_send(mongo->task, mongo->fd, mongo->skid, startsession, lens, NULL, 0);
    if (NULL == mgpack) {
        mongo_set_error(mongo, "send startsession message error.", 1);
        return NULL;
    }
    mongo_session *session;
    MALLOC(session, sizeof(mongo_session));
    if (!mongo_parse_startsession(mongo, mgpack, session->uuid, &session->timeoutmin)) {
        FREE(session);
        return NULL;
    }
    session->mongo = mongo;
    session->txnnumber = 0;
    session->timeout = nowsec() + session->timeoutmin * 60;
    return session;
}
int32_t mongo_refreshsession(mongo_session *session) {
    mongo_ctx *mongo = session->mongo;
    int32_t flags = mongo_clear_flag(mongo);
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *refreshsession = mongo_pack_refreshsession(session, &lens);
    mongo_set_flag(mongo, flags);
    mgopack_ctx *mgpack = coro_send(mongo->task, mongo->fd, mongo->skid, refreshsession, lens, NULL, 0);
    if (NULL == mgpack) {
        mongo_set_error(mongo, "send refreshsessions message error.", 1);
        return ERR_FAILED;
    }
    if (ERR_FAILED == mongo_parse_check_error(mongo, mgpack)) {
        return ERR_FAILED;
    }
    session->timeout = nowsec() + session->timeoutmin * 60;
    return ERR_OK;
}
void mongo_freesession(mongo_session *session) {
    mongo_ctx *mongo = session->mongo;
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *endsession = mongo_pack_endsession(session, &lens);
    _mongo_send(mongo, "endsessions", endsession, lens, NULL);
    FREE(session->options);
    FREE(session);
}
void mongo_begin(mongo_session *session) {
    mongo_ctx *mongo = session->mongo;
    session->txnnumber++;
    session->options = mongo_transaction_options(session);
    mongo->session = session;
}
int32_t mongo_commit(mongo_session *session, char *options) {
    mongo_ctx *mongo = session->mongo;
    int32_t flags = mongo_clear_flag(mongo);
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *committransaction = mongo_pack_committransaction(session, options, &lens);
    mongo_set_flag(mongo, flags);
    mongo->session = NULL;
    FREE(session->options);
    mgopack_ctx *mgpack = coro_send(mongo->task, mongo->fd, mongo->skid, committransaction, lens, NULL, 0);
    if (NULL == mgpack) {
        mongo_set_error(mongo, "send committransaction message error.", 1);
        return ERR_FAILED;
    }
    if (ERR_FAILED == mongo_parse_check_error(mongo, mgpack)) {
        return ERR_FAILED;
    }
    session->timeout = nowsec() + session->timeoutmin * 60;
    return ERR_OK;
}
int32_t mongo_rollback(mongo_session *session, char *options) {
    mongo_ctx *mongo = session->mongo;
    int32_t flags = mongo_clear_flag(mongo);
    mongo_set_error(mongo, NULL, 0);
    size_t lens;
    void *aborttransaction = mongo_pack_aborttransaction(session, options, &lens);
    mongo_set_flag(mongo, flags);
    mongo->session = NULL;
    FREE(session->options);
    mgopack_ctx *mgpack = coro_send(mongo->task, mongo->fd, mongo->skid, aborttransaction, lens, NULL, 0);
    if (NULL == mgpack) {
        mongo_set_error(mongo, "send aborttransaction message error.", 1);
        return ERR_FAILED;
    }
    if (ERR_FAILED == mongo_parse_check_error(mongo, mgpack)) {
        return ERR_FAILED;
    }
    session->timeout = nowsec() + session->timeoutmin * 60;
    return ERR_OK;
}
