#include "test_base.h"
#include "test_utils.h"
#include "task_startup_closing.h"
#include "task_timeout.h"
#include "task_tcp.h"
#include "task_udp.h"
#include "task_thread_comm1.h"
#include "task_thread_comm2.h"
#include "task_ssl.h"
#include "task_wbsock_sv.h"
#include "task_http_sv.h"
#include "task_redis.h"
#include "task_coro_timeout.h"
#include "task_coro_comm1.h"
#include "task_coro_net.h"
#include "task_coro_utils.h"
#include "task_mysql.h"

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
static void on_sigcb(int32_t sig, void *arg) {
    LOG_INFO("catch sign: %d", sig);
    cond_signal(&condexit);
}
int main(int argc, char *argv[]) {
    unlimit();
    srand((unsigned int)time(NULL)); 
    log_init(NULL);
    mutex_init(&muexit);
    cond_init(&condexit);
    sighandle(on_sigcb, NULL);
    dns_set_ip("8.8.8.8");

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

    g_scheduler = scheduler_init(1, 2);
#if WITH_SSL
    const char *local = procpath();
    char ca[PATH_LENS] = { 0 };
    char svcrt[PATH_LENS] = { 0 };
    char svkey[PATH_LENS] = { 0 };
    char p12[PATH_LENS] = { 0 };
    SNPRINTF(ca, sizeof(ca) - 1, "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "ca.crt");
    SNPRINTF(svcrt, sizeof(svcrt) - 1, "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "sever.crt");
    SNPRINTF(svkey, sizeof(svkey) - 1, "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "sever.key");
    evssl_ctx *ssl = evssl_new(ca, svcrt, svkey, SSL_FILETYPE_PEM, 0);
    srey_ssl_register(g_scheduler, 100, ssl);
    SNPRINTF(p12, sizeof(p12) - 1, "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "client.p12");
    ssl = evssl_p12_new(p12, "srey", 0);
    srey_ssl_register(g_scheduler, 101, ssl);
    ssl = evssl_new(NULL, NULL, NULL, SSL_FILETYPE_PEM, 0);
    srey_ssl_register(g_scheduler, 102, ssl);
#endif
    /*task_startup_closing_start(g_scheduler, 10000, 0);
    task_timeout_start(g_scheduler, 10001, 0);
    task_tcp_start(g_scheduler, 10002, 0);
    task_udp_start(g_scheduler, 10003, 0);
    task_threadcomm1_start(g_scheduler, 10004, 0);
    task_threadcomm2_start(g_scheduler, 10005, 0);*/
#if WITH_SSL
    //task_ssl_start(g_scheduler, 10006, srey_ssl_qury(g_scheduler, 100), 0);
    //10007 task_auto_close
#endif
    /*task_wbsock_sv_start(g_scheduler, 10008, 0);
    task_http_sv_start(g_scheduler, 10009, 0);*/
#if WITH_CORO
    /*task_coro_timeout_start(g_scheduler, 20000, 0);
    task_coro_comm1_start(g_scheduler, 20001, 0);
    task_coro_net_start(g_scheduler, 20002, 0);
    task_coro_utils_start(g_scheduler, 20003, 0);
    task_redis_start(g_scheduler, 20004, 0);*/
    task_mysql_start(g_scheduler, 20005, 1);
#endif
    mutex_lock(&muexit);
    cond_wait(&condexit, &muexit);
    mutex_unlock(&muexit);
    scheduler_free(g_scheduler);
    mutex_free(&muexit);
    cond_free(&condexit);
    _memcheck();
    return 0;
}
