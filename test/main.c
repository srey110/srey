#include "test_base.h"
#include "test_utils.h"
#include "test_timeout.h"
#include "test_tcp.h"
#include "test_ssl.h"
#include "test_udp.h"
#include "test_http.h"
#include "test_wbsk.h"

#ifdef OS_WIN
//#include "vld.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#if WITH_SSL
#ifdef ARCH_X64
#pragma comment(lib, "libcrypto_x64.lib")
#pragma comment(lib, "libssl_x64.lib")
#else
#pragma comment(lib, "libcrypto.lib")
#pragma comment(lib, "libssl.lib")
#endif
#endif
#endif

mutex_ctx muexit;
cond_ctx condexit;
srey_ctx *srey = NULL;
#define START_ONLY_EV 1
#if START_ONLY_EV
static int32_t test_acpt_cb(ev_ctx *ctx, SOCKET sock, uint64_t skid, ud_cxt *ud) {
    return ERR_OK;
}
static void test_recv_cb(ev_ctx *ctx, SOCKET sock, uint64_t skid, buffer_ctx *buf, size_t lens, ud_cxt *ud) {
    if (randrange(1, 100) <= 1) {
        ev_close(ctx, sock, skid);
        return;
    }
    size_t len = buffer_size(buf);
    char *pk;
    MALLOC(pk, len);
    buffer_remove(buf, pk, len);
    ev_send(ctx, sock, skid, pk, len, 0);
}
static void test_recvfrom_cb(ev_ctx *ev, SOCKET fd, uint64_t skid, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    char host[IP_LENS] = { 0 };
    netaddr_ip(addr, host);
    uint16_t port = netaddr_port(addr);
    ev_sendto(ev, fd, skid, host, port, buf, size);
}
#endif
static void on_sigcb(int32_t sig, void *arg) {
    PRINT("catch sign: %d", sig);
    cond_signal(&condexit);
}
int main(int argc, char *argv[]) {
    cJSON_Hooks hooks;
    hooks.malloc_fn = _malloc;
    hooks.realloc_fn = _realloc;
    hooks.free_fn = _free;
    cJSON_InitHooks(&hooks);
    unlimit();
    srand((unsigned int)time(NULL)); 
    mutex_init(&muexit);
    cond_init(&condexit);
    sighandle(on_sigcb, NULL);

    CuString *poutput = CuStringNew();
    CuSuite* psuite = CuSuiteNew();
    test_base(psuite);
    test_utils(psuite);
    CuSuiteRun(psuite);
    CuSuiteSummary(psuite, poutput);
    CuSuiteDetails(psuite, poutput);
    printf("%s\n", poutput->buffer);  
    CuStringDelete(poutput);
    CuSuiteDelete(psuite);

    PRINT("-------------------------------------------------");
#if START_ONLY_EV
    ev_ctx ev;
    ev_init(&ev, 2);
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.acp_cb = test_acpt_cb;
    cbs.r_cb = test_recv_cb;
    cbs.rf_cb = test_recvfrom_cb;
#endif

    srey = srey_init(2, 2, 0, "p1KaLmIg2kB7fZp5Tm1qbi6dss92C31aPGJK9Lvk7DMPDONXrwD2yVLZrHBio74s");
#if WITH_SSL
    const char *local = procpath();
    char ca[PATH_LENS] = { 0 };
    char svcrt[PATH_LENS] = { 0 };
    char svkey[PATH_LENS] = { 0 };
    char p12[PATH_LENS] = { 0 };
    SNPRINTF(ca, sizeof(ca) - 1, "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "ca.crt");
    SNPRINTF(svcrt, sizeof(svcrt) - 1, "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "sever.crt");
    SNPRINTF(svkey, sizeof(svkey) - 1, "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "sever.key");
    SNPRINTF(p12, sizeof(p12) - 1, "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "client.p12");
    evssl_ctx *ssl = evssl_new(ca, svcrt, svkey, SSL_FILETYPE_PEM, 0);
    if (ERR_OK != srey_ssl_register(srey, SSL_SERVER, ssl)) {
        PRINT("srey_ssl_register error.");
    }
#if START_ONLY_EV
    ev_listen(&ev, ssl, "0.0.0.0", 16001, &cbs, NULL, NULL);
#endif
    ssl = evssl_p12_new(p12, "srey", 0);
    if (ERR_OK != srey_ssl_register(srey, SSL_CLINET, ssl)) {
        PRINT("srey_ssl_register error.");
    }
#endif
    test_timeout();
    test_tcp();
    test_ssl();
    test_udp();
    test_httpsv();
    test_wbsk();
    harbor_start(srey, 1, 0, "0.0.0.0", 8080);
#if START_ONLY_EV
    ev_listen(&ev, NULL, "0.0.0.0", 16000, &cbs, NULL, NULL);
    uint64_t skid;
    ev_udp(&ev, "0.0.0.0", 16002, &cbs, NULL, &skid);
#endif
    mutex_lock(&muexit);
    cond_wait(&condexit, &muexit);
    mutex_unlock(&muexit);
    srey_free(srey);
#if START_ONLY_EV
    ev_free(&ev);
#endif
    mutex_free(&muexit);
    cond_free(&condexit);
    _memcheck();
    return 0;
}
