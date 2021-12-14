#include "lib.h"

#ifdef OS_WIN
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#endif

mutex_ctx g_muexit;
cond_ctx g_condexit;

int32_t startup()
{
    lua_State *plua = lua_newfile("startup.lua");
    if (NULL == plua)
    {
        return ERR_FAILED;
    }
    lua_close(plua);
    return ERR_OK;
}
void sig_cb(int32_t isig, void *pud)
{
    LOG_INFO("catch signal %d.", isig);
    struct srey_ctx *psrey = pud;
    srey_free(psrey);
    LOGFREE();
    cond_signal(&g_condexit);
}
int main(int argc, char *argv[])
{
    lua_initpath();
    unlimit();
    mutex_init(&g_muexit);
    cond_init(&g_condexit);
    LOGINIT();
    SETLOGPRT(1);
    g_srey = srey_new(0, FREE, 5000);
    sighandle(sig_cb, g_srey);
    srey_loop(g_srey);
    if (ERR_OK != startup())
    {
        srey_free(g_srey);
        LOGFREE();
        mutex_free(&g_muexit);
        cond_free(&g_condexit);
        return ERR_FAILED;
    }
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
