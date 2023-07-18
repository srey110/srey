#include "test5.h"

static SOCKET _udp_fd = INVALID_SOCK;
static uint64_t _udp_skid;
static int32_t _handshaked = 0;
static SOCKET _wbsk_fd = INVALID_SOCK;
static uint64_t _wbsk_skid;

#if WITH_CORO
static void _test_synsend(task_ctx *task) {
    SOCKET fd;
    uint64_t skid;
    struct evssl_ctx *ssl = srey_ssl_qury(task->srey, SSL_CLINET);
    fd = syn_connect(task, PACK_SIMPLE, ssl, "127.0.0.1", 15001, 0, &skid);
    if (INVALID_SOCK == fd) {
        LOG_WARN("syn_netconnect error.");
        return;
    }
    const char *sdata = "this is syn_synsend.";
    size_t rlen;
    uint64_t sess = createid();
    void *spack = simple_pack((void *)sdata, strlen(sdata), &rlen);
    char *data = syn_send(task, fd, skid, sess, spack, rlen, &rlen, 0);
    ev_close(&task->srey->netev, fd, skid);
    if (NULL == data) {
        LOG_WARN("syn_synsend error.");
        return;
    }
    char *simple = simple_data((struct simple_pack_ctx *)data, &rlen);
    if (rlen != strlen(sdata)
        || 0 != memcmp(simple, sdata, rlen)) {
        LOG_WARN("syn_synsend error.");
    }
}
static void _test_synsendto(task_ctx *task) {
    SOCKET fd;
    uint64_t skid;
    fd = srey_udp(task, "0.0.0.0", 0, &skid);
    if (INVALID_SOCK == fd) {
        LOG_WARN("srey_udp error.");
        return;
    }
    const char *sdata = "this is syn_synsendto.";
    size_t rlen;
    char *data = syn_sendto(task, fd, skid, "127.0.0.1", 15002, (void *)sdata, strlen(sdata), &rlen);
    ev_close(&task->srey->netev, fd, skid);
    if (NULL == data) {
        LOG_WARN("syn_synsendto error.");
        return;
    }
    if (rlen != strlen(sdata)
        || 0 != memcmp(data, sdata, rlen)) {
        LOG_WARN("syn_synsendto error.");
    }
}
#endif
static void _print_dns(dns_ip *ips, size_t cnt) {
    for (size_t i = 0; i < cnt; i++) {
        LOG_INFO("%s", ips[i].ip);
    }
}
static void _test_dns(task_ctx *task) {
#if WITH_CORO
    size_t cnt;
    dns_ip *ips = syn_dns_lookup(task, "8.8.8.8", "www.google.com", 0, &cnt);
    if (NULL == ips) {
        LOG_WARN("syn_dns_lookup error.");
    } else {
        _print_dns(ips, cnt);
        FREE(ips);
    }
    ips = syn_dns_lookup(task, "8.8.8.8", "www.google.com", 1, &cnt);
    if (NULL == ips) {
        LOG_WARN("syn_dns_lookup error.");
    } else {
        _print_dns(ips, cnt);
        FREE(ips);
    }
#else
    if (INVALID_SOCK == _udp_fd) {
        _udp_fd = srey_udp(task, "0.0.0.0", 0, &_udp_skid);
        if (INVALID_SOCK == _udp_fd) {
            LOG_WARN("srey_udp error.");
            return;
        }
    }    
    char dnsreq[ONEK] = { 0 };//±ØÐëÖÃ0
    size_t lens = dns_request_pack(dnsreq, "www.google.com", 0);
    ev_sendto(&task->srey->netev, _udp_fd, _udp_skid, "8.8.8.8", 53, dnsreq, lens);
    lens = dns_request_pack(dnsreq, "www.google.com", 1);
    ev_sendto(&task->srey->netev, _udp_fd, _udp_skid, "8.8.8.8", 53, dnsreq, lens);
#endif
}
static void test_websk_connet(task_ctx *task) {
    if (INVALID_SOCK != _wbsk_fd) {
        return;
    }
#if WITH_CORO
    _wbsk_fd = syn_websock_connect(task, "124.222.224.186", 8800, NULL, &_wbsk_skid);
    if (INVALID_SOCK != _wbsk_fd) {
        _handshaked = 1;
    }
#else
    _wbsk_fd = srey_connect(task, 0, PACK_WEBSOCK, NULL, "124.222.224.186", 8800, 0, &_wbsk_skid);
#endif
}
void test5_run(task_ctx *task, message_ctx *msg) {
    switch (msg->mtype) {
    case MSG_TYPE_STARTUP: {
        _test_dns(task);
#if WITH_CORO
        syn_timeout(task, createid(), 100);
#else
        srey_timeout(task, createid(), 100);
#endif
        break;
    }
    case MSG_TYPE_TIMEOUT: {
        test_websk_connet(task);
#if WITH_CORO
        _test_synsend(task);
        _test_synsendto(task);
        syn_timeout(task, createid(), 3000);
#else
        srey_timeout(task, createid(), 3000);
#endif
        if (0 != _handshaked) {
            websock_ping(&task->srey->netev, _wbsk_fd, _wbsk_skid, 1);
            const char *wdata = "this is websocket text continuation 0";
            websock_text(&task->srey->netev, _wbsk_fd, _wbsk_skid, 1, 0, (void *)wdata, strlen(wdata));
            websock_continuation(&task->srey->netev, _wbsk_fd, _wbsk_skid, 1, 0, "1", 1);
            websock_continuation(&task->srey->netev, _wbsk_fd, _wbsk_skid, 1, 0, "2", 1);
            websock_continuation(&task->srey->netev, _wbsk_fd, _wbsk_skid, 1, 1, "3", 1);
        }
        break;
    }
    case MSG_TYPE_CONNECT: {
        char *hs = websock_handshake_pack("124.222.224.186:8800");
        ev_send(&task->srey->netev, _wbsk_fd, _wbsk_skid, hs, strlen(hs), 0);
        break;
    }
    case MSG_TYPE_RECV: {
        //LOG_INFO("websock proto %d.", websock_pack_proto(msg->data));
        size_t lens;
        char *data = websock_pack_data(msg->data, &lens);
        if (0 != lens) {
            //LOG_INFO("websock data lens %d.", (uint32_t)lens);
        }
        break;
    }
    case MSG_TYPE_HANDSHAKED: {
        _handshaked = 1;
        break;
    }
    case MSG_TYPE_CLOSE: {
        if (_wbsk_skid == msg->skid) {
            PRINT("websocket connction closed.");
            _wbsk_skid = INVALID_SOCK;
        }
        break;
    }
    case MSG_TYPE_RECVFROM: {
        size_t cnt;
        dns_ip *ips = dns_parse_pack(((char *)msg->data) + sizeof(netaddr_ctx), &cnt);
        _print_dns(ips, cnt);
        FREE(ips);
        break;
    }
    default:
        break;
    }
}
