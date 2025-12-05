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
#include "task_pgsql.h"
#include "task_mongo.h"
#include "task_smtp.h"
#include "task_mqttsv.h"
#include "task_mqttclient.h"

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
static void on_sigcb(int32_t sig, void *arg) {
    LOG_INFO("catch sign: %d", sig);
    cond_signal(&condexit);
}
int main(int argc, char *argv[]) {
    unlimit();
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

    g_loader = loader_init(0, 0);
#if WITH_SSL
    const char *local = procpath();
    char ca[PATH_LENS];
    char svcrt[PATH_LENS];
    char svkey[PATH_LENS];
    char p12[PATH_LENS];
    SNPRINTF(ca, sizeof(ca), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "ca.crt");
    SNPRINTF(svcrt, sizeof(svcrt), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "sever.crt");
    SNPRINTF(svkey, sizeof(svkey), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "sever.key");
    evssl_ctx *ssl = evssl_new(ca, svcrt, svkey, SSL_FILETYPE_PEM);
    evssl_register(100, ssl);
    SNPRINTF(p12, sizeof(p12), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "client.p12");
    ssl = evssl_p12_new(p12, "srey");
    evssl_register(101, ssl);
    ssl = evssl_new(NULL, NULL, NULL, SSL_FILETYPE_PEM);
    evssl_register(102, ssl);
#endif
    task_startup_closing_start(g_loader, 10000, 0);
    task_timeout_start(g_loader, 10001, 0);
    task_tcp_start(g_loader, 10002, 0);
    task_udp_start(g_loader, 10003, 0);
    task_threadcomm1_start(g_loader, 10004, 0);
    task_threadcomm2_start(g_loader, 10005, 0);
#if WITH_SSL
    task_ssl_start(g_loader, 10006, evssl_qury(100), 0);
    //10007 task_auto_close
#endif
    task_wbsock_sv_start(g_loader, 10008, 0);
    task_http_sv_start(g_loader, 10009, 0);
    task_smtp_start(g_loader, 10010, 0);
    task_mqtt_sv_start(g_loader, 10011, 0);
    task_mqtt_client_start(g_loader, 10012, 0);
    task_pgsql_start(g_loader, 10013, 0);
    task_mongo_start(g_loader, 10014, 0);
    task_coro_timeout_start(g_loader, 20000, 0);
    task_coro_comm1_start(g_loader, 20001, 0);
    task_coro_net_start(g_loader, 20002, 0);
    task_coro_utils_start(g_loader, 20003, 0);
    task_redis_start(g_loader, 20004, 0);
    task_mysql_start(g_loader, 20005, 1);
    mutex_lock(&muexit);
    cond_wait(&condexit, &muexit);
    mutex_unlock(&muexit);
    loader_free(g_loader);
    mutex_free(&muexit);
    cond_free(&condexit);
    _memcheck();
    return 0;
}
