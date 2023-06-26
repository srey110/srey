#include "test_base.h"
#include "test_utils.h"
#include "lib.h"

#ifdef OS_WIN
#include "vld.h"
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
atomic_t count = 0;
tw_ctx tw;

static void on_sigcb(int32_t sig, void *arg) {
    PRINT("catch sign: %d", sig);
    cond_signal(&condexit);
}
static void test_close_cb(ev_ctx *ctx, SOCKET sock, ud_cxt *ud) {
    //PRINT("test_close_cb: sock %d ", (int32_t)sock);
    ATOMIC_ADD(&count, -1);
}
static int32_t test_acpt_cb(ev_ctx *ctx, SOCKET sock, ud_cxt *ud) {
    //PRINT("test_acpt_cb : sock %d ", (int32_t)sock);
    ATOMIC_ADD(&count, 1);
    return ERR_OK;
}
static void test_recv_cb(ev_ctx *ctx, SOCKET sock, buffer_ctx *buf, size_t lens, ud_cxt *ud) {
    //PRINT("test_recv_cb: lens %d ", (int32_t)lens);
    if (randrange(1, 100) <= 1) {
        ev_close(ctx, sock, ud->skid, 0);
        return;
    }
    size_t len = buffer_size(buf);
    char *pk;
    MALLOC(pk, len);
    buffer_remove(buf, pk, len);
    ev_send(ctx, sock, ud->skid, pk, len, SYN_NONE, 0, 0);
}
static void test_recvfrom_cb(ev_ctx *ev, SOCKET fd, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    char host[IP_LENS] = { 0 };
    netaddr_ip(addr, host);
    uint16_t port = netaddr_port(addr);
    ev_sendto(ev, fd, ud->skid, host, port, buf, size, 0, SYN_NONE);
}
static void timeout(ud_cxt *ud) {
    int32_t elapsed = (int32_t)timer_elapsed_ms(&tw.timer);
    PRINT("timeout:%d ms link cnt %d", elapsed, ATOMIC_GET(&count));
    timer_start(&tw.timer);
    tw_add(&tw, 3000, timeout, ud);
}

int main(int argc, char *argv[]) {
    MEMCHECK();
    unlimit();
    srand((unsigned int)time(NULL)); 
    mutex_init(&muexit);
    cond_init(&condexit);
    sighandle(on_sigcb, NULL);
    LOGINIT();    

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

    tw_init(&tw);
    ev_ctx ev;
    ev_init(&ev, 2);
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.acp_cb = test_acpt_cb;
    cbs.c_cb = test_close_cb;
    cbs.r_cb = test_recv_cb;
    cbs.rf_cb = test_recvfrom_cb;
    ev_listen(&ev, NULL, "0.0.0.0", 15000, &cbs, NULL);
    uint64_t skid;
    ev_udp(&ev, "0.0.0.0", 15002, &cbs, NULL, &skid);
#if WITH_SSL
    char local[PATH_LENS] = { 0 };
    procpath(local);
    char ca[PATH_LENS] = { 0 };
    char svcrt[PATH_LENS] = { 0 };
    char svkey[PATH_LENS] = { 0 };
    char p12[PATH_LENS] = { 0 };
    SNPRINTF(ca, sizeof(ca) - 1, "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "ca.crt");
    SNPRINTF(svcrt, sizeof(svcrt) - 1, "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "sever.crt");
    SNPRINTF(svkey, sizeof(svkey) - 1, "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "sever.key");
    SNPRINTF(p12, sizeof(p12) - 1, "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "client.p12");
    struct evssl_ctx *ssl = evssl_new(ca, svcrt, svkey, SSL_FILETYPE_PEM, 0);
    ev_listen(&ev, ssl, "0.0.0.0", 15001, &cbs, NULL);
#endif

    timer_start(&tw.timer);
    ud_cxt ud;
    ud.data = &ev;
    tw_add(&tw, 1000, timeout, &ud);

    mutex_lock(&muexit);
    cond_wait(&condexit, &muexit);
    mutex_unlock(&muexit);

    tw_free(&tw);
    PRINT("link cnt %d", ATOMIC_GET(&count));
    ev_free(&ev);

#if WITH_SSL
    evssl_free(ssl);
#endif
    mutex_free(&muexit);
    cond_free(&condexit);

    LOGFREE();
    return 0;
}
