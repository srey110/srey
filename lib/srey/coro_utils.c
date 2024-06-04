#include "srey/coro_utils.h"
#include "srey/coro.h"
#include "srey/ssls.h"
#include "trigger.h"
#include "proto/urlparse.h"
#include "proto/dns.h"
#include "proto/websock.h"
#include "proto/http.h"
#include "proto/redis.h"
#include "proto/mysql_macro.h"
#include "buffer.h"

#if WITH_CORO

dns_ip *coro_dns_lookup(task_ctx *task, const char *domain, int32_t ipv6, size_t *cnt) {
    uint64_t skid;
    SOCKET fd;
    const char *dnsip = dns_get_ip();
    if (ERR_OK == is_ipv6(dnsip)) {
        fd = trigger_udp(task, "::", 0, &skid);
    } else {
        fd = trigger_udp(task, "0.0.0.0", 0, &skid);
    }
    if (INVALID_SOCK == fd) {
        LOG_WARN("init udp error.");
        return NULL;
    }
    char buf[ONEK] = { 0 };
    size_t lens = dns_request_pack(buf, domain, ipv6);
    void *resp = coro_sendto(task, fd, skid, dnsip, 53, buf, lens, &lens);
    ev_close(&task->scheduler->netev, fd, skid);
    if (NULL == resp) {
        return NULL;
    }
    return dns_parse_pack(resp, cnt);
}
SOCKET coro_wbsock_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ws, uint64_t *skid, int32_t netev) {
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
        dns_ip *ips = coro_dns_lookup(task, host, 0, &nips);
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
    SOCKET fd = coro_connect(task, PACK_WEBSOCK, evssl, ip, port, skid, netev);
    if (INVALID_SOCK == fd) {
        FREE(host);
        return INVALID_SOCK;
    }
    char *reqpack = websock_handshake_pack(host);
    FREE(host);
    ev_send(&task->scheduler->netev, fd, *skid, reqpack, strlen(reqpack), 0);
    if (ERR_OK != coro_handshaked(task, fd, *skid)) {
        return INVALID_SOCK;
    }
    return fd;
}
SOCKET coro_redis_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ip, uint16_t port, const char *key, uint64_t *skid, int32_t netev) {
    SOCKET fd = coro_connect(task, PACK_REDIS, evssl, ip, port, skid, netev);
    if (INVALID_SOCK == fd) {
        return INVALID_SOCK;
    }
    if (NULL != key
        && 0 != strlen(key)) {
        size_t size;
        char *auth = redis_pack(&size, "AUTH %s", key);
        redis_pack_ctx *rtn = coro_send(task, fd, *skid, auth, size, &size, 0);
        if (NULL == rtn
            || RESP_STRING != rtn->proto
            || 2 != rtn->len
            || 0 != _memicmp(rtn->data, "ok", (size_t)rtn->len)) {
            ev_close(&task->scheduler->netev, fd, *skid);
            return INVALID_SOCK;
        }
    }
    return fd;
}
int32_t mysql_connect(task_ctx *task, mysql_ctx *mysql) {
    if (ERR_OK != mysql_try_connect(task, mysql)) {
        return ERR_FAILED;
    }
    return coro_handshaked(task, mysql->client.fd, mysql->client.skid);
}
void mysql_quit(task_ctx *task, mysql_ctx *mysql) {
    size_t size;
    void *quit = mysql_pack_quit(mysql, &size);
    if (NULL != quit) {
        coro_send(task, mysql->client.fd, mysql->client.skid, quit, size, &size, 0);
    }
}
int32_t mysql_selectdb(task_ctx *task, mysql_ctx *mysql, const char *database) {
    size_t size;
    void *selectdb = mysql_pack_selectdb(mysql, database, &size);
    if (NULL == selectdb) {
        return ERR_FAILED;
    }
    mpack_ctx *mpack = coro_send(task, mysql->client.fd, mysql->client.skid, selectdb, size, &size, 0);
    if (NULL == mpack) {
        return ERR_FAILED;
    }
    if (MYSQL_OK == mpack->command) {
        return ERR_OK;
    }
    return ERR_FAILED;
}
int32_t mysql_ping(task_ctx *task, mysql_ctx *mysql) {
    size_t size;
    void *ping = mysql_pack_ping(mysql, &size);
    if (NULL == ping) {
        return ERR_FAILED;
    }
    mpack_ctx *mpack = coro_send(task, mysql->client.fd, mysql->client.skid, ping, size, &size, 0);
    if (NULL == mpack) {
        return ERR_FAILED;
    }
    if (MYSQL_OK == mpack->command) {
        return ERR_OK;
    }
    return ERR_FAILED;
}

#endif
