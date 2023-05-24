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

tw_ctx tw;
mutex_ctx muexit;
cond_ctx condexit;
int32_t pk_index = 0;
SOCKET connsock = INVALID_SOCK;
SOCKET udpsock = INVALID_SOCK;
static atomic_t count = 0;
#if WITH_SSL
struct evssl_ctx *ssl_client;
#endif
static void on_sigcb(int32_t sig, void *arg) {
    PRINT("catch sign: %d", sig);
    cond_signal(&condexit);
}
static void test_close_cb(ev_ctx *ctx, SOCKET sock, ud_cxt *ud) {
    //PRINT("test_close_cb: sock %d ", (int32_t)sock);
    ATOMIC_ADD(&count, -1);
}
static void test_connclose_cb(ev_ctx *ctx, SOCKET sock, ud_cxt *ud) {
    //PRINT("test_connclose_cb: sock %d ", (int32_t)sock);
    connsock = INVALID_SOCK;
}
static void test_recv_cb(ev_ctx *ctx, SOCKET sock, buffer_ctx *buf, size_t lens, ud_cxt *ud) {
    //PRINT("test_recv_cb: lens %d ", (int32_t)lens);
    //if (randrange(1, 100) <= 1) {
    //    //PRINT("close socket: sock %d ", (int32_t)sock);
    //    ev_close(ctx, sock);
    //    return;
    //}
    size_t len = buffer_size(buf);    
    char *pk;
    MALLOC(pk, len);
    buffer_remove(buf, pk, len);
    ev_send(ctx, sock, pk, len, 0);
}
static void test_send_cb(ev_ctx *ctx, SOCKET sock, size_t len, ud_cxt *ud) {
    //PRINT("test_send_cb: sock %d  len %d ", (int32_t)sock, (int32_t)len);
}
static void test_conn_recv_cb(ev_ctx *ctx, SOCKET sock, buffer_ctx *buf, size_t lens, ud_cxt *ud) {
    //PRINT("test_conn_recv_cb: lens %d", (int32_t)lens);
    if (buffer_size(buf) <= 2 + sizeof(pk_index)) {
        return;
    }
    char len[2 + sizeof(pk_index)];
    buffer_copyout(buf, len, sizeof(len));
    u_short *plen = (u_short*)len;
    u_short pklen = ntohs(*plen);
    if (buffer_size(buf) < pklen) {
        return;
    }
    int32_t pkindex = *(int32_t*)(len + 2);
    int32_t tmp = (int32_t)(ntohl(pkindex));
    if (tmp != pk_index) {
        PRINT("index error.recv: %d  cur %d", tmp, pk_index);
    }
    buffer_drain(buf, buffer_size(buf));
}
static int32_t test_conn_cb(ev_ctx *ctx, SOCKET sock, int32_t err, ud_cxt *ud) {
    if (ERR_OK == err) {
        PRINT("%s", "connect ok.");
        connsock = sock;
    }
    else {
        PRINT("%s", "connect error.");
    }
    return ERR_OK;
}
static int32_t test_acpt_cb(ev_ctx *ctx, SOCKET sock, ud_cxt *ud) {
    //PRINT("test_acpt_cb : sock %d ", (int32_t)sock);
    ATOMIC_ADD(&count, 1);
    return ERR_OK;
}
static void test_recvfrom_cb(ev_ctx *ev, SOCKET fd, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud) {
    char host[IP_LENS] = { 0 };
    netaddr_ip(addr, host);
    uint16_t port = netaddr_port(addr);
    ev_sendto(ev, fd, host, port, buf, size);
}
static void timeout(ud_cxt *ud) {
    int32_t elapsed = (int32_t)timer_elapsed_ms(&tw.timer);
    PRINT("timeout:%d ms link cnt %d", elapsed, ATOMIC_GET(&count));
    /*if (INVALID_SOCK != udpsock) {
        ev_close(ud->data, udpsock);
        udpsock = INVALID_SOCK;
    } else {
        udpsock = ev_udp(ud->data, "0.0.0.0", 15002, test_recvfrom_cb, NULL);
    }*/
    if (INVALID_SOCK != connsock) {
        /*ev_close(arg, connsock);
        connsock = INVALID_SOCK;*/
        char str[100];
        int32_t len = randrange(1, sizeof(str));
        ASSERTAB(sizeof(str) > len, "randrange error.");
        randstr(str, len);
        u_short total = (u_short)(2 + sizeof(pk_index) + len);
        char *buf;
        MALLOC(buf, total);
        pk_index++;
        //PRINT("send pack index: %d", pk_index);
        total = ntohs(total);
        memcpy(buf, &total, sizeof(total));
        int32_t tmp = ntohl(pk_index);
        memcpy(buf + sizeof(total), &tmp, sizeof(tmp));
        memcpy(buf + sizeof(total) + sizeof(pk_index), str, len);
        ev_send(ud->data, connsock, buf, 2 + sizeof(pk_index) + len, 0);

        /*ev_sendto(ud->data, connsock, "127.0.0.1", 15002, buf, 2 + sizeof(pk_index) + len, 0);
        FREE(buf);*/
    } else {
        cbs_ctx cbs;
        ZERO(&cbs, sizeof(cbs));
        cbs.conn_cb = test_conn_cb;
        cbs.c_cb = test_connclose_cb;
        cbs.r_cb = test_conn_recv_cb;
        cbs.s_cb = test_send_cb;
#if WITH_SSL
        ev_connect(ud->data, ssl_client, "127.0.0.1", 15001, &cbs, NULL);
#else
        ev_connect(ud->data, NULL, "127.0.0.1", 15000, &cbs, NULL);
#endif
    }
    timer_start(&tw.timer);
    tw_add(&tw, 3000, timeout, ud);
}
#if WITH_SSL
int verify_sv_cb(int preverify_ok, X509_STORE_CTX *x509_ctx) {
    return 1;
}
int verify_clinet_cb(int preverify_ok, X509_STORE_CTX *x509_ctx) {
    return 1;
}
#endif
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
    cbs.s_cb = test_send_cb;
    ev_listen(&ev, NULL, "0.0.0.0", 15000, &cbs, NULL);
    udpsock = ev_udp(&ev, "0.0.0.0", 15002, test_recvfrom_cb, NULL);

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
    struct evssl_ctx *ssl = evssl_new(ca, svcrt, svkey, SSL_FILETYPE_PEM, NULL);
    ssl_client = evssl_p12_new(p12, "srey", NULL);
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
    if (INVALID_SOCK != udpsock) {
        ev_close(&ev, udpsock);
        MSLEEP(500);
    }
    if (INVALID_SOCK != connsock) {
        ev_close(&ev, connsock);
        MSLEEP(500);
    }
    PRINT("link cnt %d", ATOMIC_GET(&count));
    ev_free(&ev);

#if WITH_SSL
    evssl_free(ssl);
    evssl_free(ssl_client);
#endif
    mutex_free(&muexit);
    cond_free(&condexit);
    LOGFREE();

    return 0;
}
