#include "lib.h"

#ifdef OS_WIN
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#endif

mutex_ctx g_muexit;
cond_ctx g_condexit;
struct tw_ctx tw;
uint64_t ulstart = 0;

void sig_cb(int32_t isig, void *pud)
{
    LOG_INFO("catch signal %d.", isig);
    //struct srey_ctx *psrey = pud;
    //srey_free(psrey);
    tw_free(&tw);
    LOGFREE();
    cond_signal(&g_condexit);
}

void tw_cb(struct ud_ctx *pctx)
{
    int i = nowmsec() - ulstart;
    PRINTF("%d", i);
    ulstart = nowmsec();
    tw_add(&tw, 1000, tw_cb, NULL);
}

int main(int argc, char *argv[])
{
    //initpath();
    unlimit();
    mutex_init(&g_muexit);
    cond_init(&g_condexit);
    LOGINIT();
    SETLOGPRT(1);
    //g_srey = srey_new(0, FREE, 5000);
    sighandle(sig_cb, NULL);
    //srey_loop(g_srey);
    /*if (ERR_OK != lua_startup())
    {
        srey_free(g_srey);
        LOGFREE();
        mutex_free(&g_muexit);
        cond_free(&g_condexit);
        return ERR_FAILED;
    }*/
    LOG_DEBUG("%s", "111111111111");
    LOG_INFO("%s", "222222222222222");
    LOG_WARN("%s", "33333333333333333");
    LOG_ERROR("%s", "444444444444444444");
    LOG_FATAL("%s", "5555555555555555555");
    tw_init(&tw);
    ulstart = nowmsec();
    tw_add(&tw, 1000, tw_cb, NULL);

#ifndef OS_WIN
#ifdef OS_DARWIN
    PRINTF("kill -s USR1 %d, stop service.", (int32_t)getpid());
#else
    PRINTF("kill -%d %d, stop service.", SIGUSR1, (int32_t)getpid());
#endif
#endif
    mutex_lock(&g_muexit);
    cond_wait(&g_condexit, &g_muexit);
    mutex_unlock(&g_muexit);

    mutex_free(&g_muexit);
    cond_free(&g_condexit);

    return 0;
}
