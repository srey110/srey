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
    void *resp = coro_sendto(task, fd, skid, dnsip, 53, buf, lens, &lens);
    ev_close(&task->loader->netev, fd, skid);
    if (NULL == resp) {
        return NULL;
    }
    return dns_parse_pack(resp, cnt);
}
SOCKET wbsock_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ws, const char *secprot, uint64_t *skid, int32_t netev) {
    url_ctx url;
    url_parse(&url, (char *)ws, strlen(ws));
    if (!buf_icompare(&url.scheme, "ws", strlen("ws"))) {
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
    ev_send(&task->loader->netev, fd, *skid, reqpack, strlen(reqpack), 0);
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
        redis_pack_ctx *rtn = coro_send(task, fd, *skid, auth, size, &size, 0);
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
    return err;
}
int32_t mysql_selectdb(mysql_ctx *mysql, const char *database) {
    size_t size;
    void *selectdb = mysql_pack_selectdb(mysql, database, &size);
    if (NULL == selectdb) {
        return ERR_FAILED;
    }
    mpack_ctx *mpack = coro_send(mysql->task, mysql->client.fd, mysql->client.skid, selectdb, size, &size, 0);
    if (NULL == mpack) {
        return ERR_FAILED;
    }
    return MPACK_OK == mpack->pack_type ? ERR_OK : ERR_FAILED;
}
static int32_t _mysql_ping(mysql_ctx *mysql) {
    size_t size;
    void *ping = mysql_pack_ping(mysql, &size);
    if (NULL == ping) {
        return ERR_FAILED;
    }
    mpack_ctx *mpack = coro_send(mysql->task, mysql->client.fd, mysql->client.skid, ping, size, &size, 0);
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
    if (NULL == query) {
        return NULL;
    }
    return coro_send(mysql->task, mysql->client.fd, mysql->client.skid, query, size, &size, 0);
}
mysql_stmt_ctx *mysql_stmt_prepare(mysql_ctx *mysql, const char *sql) {
    size_t size;
    void *prepare = mysql_pack_stmt_prepare(mysql, sql, &size);
    if (NULL == prepare) {
        return NULL;
    }
    mpack_ctx *mpack = coro_send(mysql->task, mysql->client.fd, mysql->client.skid, prepare, size, &size, 0);
    return mysql_stmt_init(mpack);
}
mpack_ctx *mysql_stmt_execute(mysql_stmt_ctx *stmt, mysql_bind_ctx *mbind) {
    size_t size;
    void *exec = mysql_pack_stmt_execute(stmt, mbind, &size);
    if (NULL == exec) {
        return NULL;
    }
    return coro_send(stmt->mysql->task, stmt->mysql->client.fd, stmt->mysql->client.skid, exec, size, &size, 0);
}
int32_t mysql_stmt_reset(mysql_stmt_ctx *stmt) {
    size_t size;
    void *resetpk = mysql_pack_stmt_reset(stmt, &size);
    if (NULL == resetpk) {
        return ERR_FAILED;
    }
    mpack_ctx *mpack = coro_send(stmt->mysql->task, stmt->mysql->client.fd, stmt->mysql->client.skid, resetpk, size, &size, 0);
    if (NULL == mpack) {
        return ERR_FAILED;
    }
    return MPACK_OK == mpack->pack_type ? ERR_OK : ERR_FAILED;
}
void mysql_quit(mysql_ctx *mysql) {
    size_t size;
    void *quit = mysql_pack_quit(mysql, &size);
    if (NULL == quit) {
        ev_close(&mysql->task->loader->netev, mysql->client.fd, mysql->client.skid);
        return;
    }
    coro_send(mysql->task, mysql->client.fd, mysql->client.skid, quit, size, &size, 0);
}
int32_t smtp_connect(task_ctx *task, smtp_ctx *smtp) {
    if (ERR_OK != smtp_try_connect(task, smtp)) {
        return ERR_FAILED;
    }
    int32_t err;
    char *msg = (char *)coro_handshaked(task, smtp->fd, smtp->skid, &err, NULL);
    if (ERR_OK != err
        && NULL != msg) {
        LOG_WARN("%s", msg);
    }
    return err;
}
static int32_t _smtp_reset(smtp_ctx *smtp) {
    if (ERR_OK != smtp_check_auth(smtp)) {
        return ERR_FAILED;
    }
    size_t size;
    char *cmd = smtp_pack_reset(smtp);
    if (NULL == cmd) {
        return ERR_FAILED;
    }
    char *pack = coro_send(smtp->task, smtp->fd, smtp->skid, cmd, strlen(cmd), &size, 0);
    if (NULL == pack) {
        return ERR_FAILED;
    }
    return smtp_check_ok(pack);
}
static void _smtp_quit(smtp_ctx *smtp) {
    size_t size;
    char *cmd = smtp_pack_quit(smtp);
    if (NULL == cmd) {
        return;
    }
    char *pack = coro_send(smtp->task, smtp->fd, smtp->skid, cmd, strlen(cmd), &size, 0);
    if (NULL == pack) {
        return;
    }
    smtp_check_code(pack, "221");
}
void smtp_quit(smtp_ctx *smtp) {
    _smtp_quit(smtp);
    ev_close(&smtp->task->loader->netev, smtp->fd, smtp->skid);
}
static int32_t _smtp_ping(smtp_ctx *smtp) {
    size_t size;
    char *cmd = smtp_pack_ping(smtp);
    if (NULL == cmd) {
        return ERR_FAILED;
    }
    char *pack = coro_send(smtp->task, smtp->fd, smtp->skid, cmd, strlen(cmd), &size, 0);
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
static int32_t _smtp_send(smtp_ctx *smtp, mail_ctx *mail) {
    size_t size;
    char *cmd = smtp_pack_from(smtp, mail->from.addr);
    if (NULL == cmd) {
        return ERR_FAILED;
    }
    char *pack = coro_send(smtp->task, smtp->fd, smtp->skid, cmd, strlen(cmd), &size, 0);
    if (NULL == pack
        || ERR_OK != smtp_check_ok(pack)) {
        return ERR_FAILED;
    }
    uint32_t naddr = arr_mail_addr_size(&mail->addrs);
    for (uint32_t i = 0; i < naddr; i++) {
        cmd = smtp_pack_rcpt(smtp, arr_mail_addr_at(&mail->addrs, i)->addr);
        if (NULL == cmd) {
            return ERR_FAILED;
        }
        pack = coro_send(smtp->task, smtp->fd, smtp->skid, cmd, strlen(cmd), &size, 0);
        if (NULL == pack
            || ERR_OK != smtp_check_ok(pack)) {
            return ERR_FAILED;
        }
    }
    cmd = smtp_pack_data(smtp);
    if (NULL == cmd) {
        return ERR_FAILED;
    }
    pack = coro_send(smtp->task, smtp->fd, smtp->skid, cmd, strlen(cmd), &size, 0);
    if (NULL == pack) {
        return ERR_FAILED;
    }
    if (ERR_OK != smtp_check_code(pack, "354")) {
        return ERR_FAILED;
    }
    cmd = mail_pack(mail);
    if (NULL == cmd) {
        return ERR_FAILED;
    }
    pack = coro_send(smtp->task, smtp->fd, smtp->skid, cmd, strlen(cmd), &size, 0);
    if (NULL == pack
        || ERR_OK != smtp_check_ok(pack)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t smtp_send(smtp_ctx *smtp, mail_ctx *mail) {
    if (ERR_OK != smtp_check_auth(smtp)) {
        return ERR_FAILED;
    }
    int32_t rtn = _smtp_send(smtp, mail);
    _smtp_reset(smtp);
    return rtn;
}
int32_t pgsql_connect(task_ctx *task, pgsql_ctx *pg) {
    if (ERR_OK != pgsql_try_connect(task, pg)) {
        return ERR_FAILED;
    }
    int32_t err;
    coro_handshaked(task, pg->fd, pg->skid, &err, NULL);
    return err;
}
void pgsql_quit(pgsql_ctx *pg) {
    size_t lens;
    void *quit = pgsql_pack_quit(pg, &lens);
    if (NULL == quit) {
        ev_close(&pg->task->loader->netev, pg->fd, pg->skid);
        return;
    }
    size_t size;
    coro_send(pg->task, pg->fd, pg->skid, quit, lens, &size, 0);
}
int32_t pgsql_ping(pgsql_ctx *pg) {
    if (0 == pg->status) {
        return pgsql_connect(pg->task, pg);
    }
    return ERR_OK;
}
