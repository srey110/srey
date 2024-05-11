#include "srey/coro_utils.h"
#include "srey/coro.h"
#include "trigger.h"
#include "proto/dns.h"
#include "proto/websock.h"
#include "proto/http.h"
#include "buffer.h"

#if WITH_CORO

dns_ip *coro_dns_lookup(task_ctx *task, int32_t ms, const char *dns, const char *domain, int32_t ipv6, size_t *cnt) {
    uint64_t skid;
    SOCKET fd;
    if (ERR_OK == is_ipv6(dns)) {
        fd = trigger_udp(task, "::", 0, &skid);
    } else {
        fd = trigger_udp(task, "0.0.0.0", 0, &skid);
    }
    if (INVALID_SOCK == fd) {
        return NULL;
    }
    char buf[ONEK] = { 0 };
    size_t lens = dns_request_pack(buf, domain, ipv6);
    void *resp = coro_sendto(task, ms, fd, skid, dns, 53, buf, lens, &lens);
    ev_close(&task->scheduler->netev, fd, skid);
    if (NULL == resp) {
        return NULL;
    }
    return dns_parse_pack(resp, cnt);
}

#endif
