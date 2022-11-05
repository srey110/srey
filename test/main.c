#include "test_base.h"
#include "test_utils.h"
#include "lib.h"

#ifdef OS_WIN
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#endif

ev_ctx ev;
mutex_ctx muexit;
cond_ctx condexit;
static uint32_t count = 0;

static void on_sigcb(int32_t sig, void *arg)
{
    PRINT("catch sign: %d", sig);
    cond_signal(&condexit);
}
static void timeout(void *arg)
{
    count++;
    tw_ctx *tw = arg;
    PRINT("timeout:%"PRIu64"", timer_elapsed(&tw->timer)/ (1000 * 1000));
    timer_start(&tw->timer);
    if (count < 2)
    {
        tw_add(tw, 1000, timeout, tw);
    }
}
void test_close_cb(SOCKET sock, void *ud)
{

}
void test_recv_cb(SOCKET sock, buffer_ctx *buf, void *ud)
{

}
void test_send_cb(SOCKET sock, void *data, size_t len, void *ud, int32_t rest)
{

}
static void acpt_cb(SOCKET sock, void *ud)
{
    uint32_t *cnt = ud;
    PRINT("acpt_cb: %d", *cnt);
    const char *str = "this is test.";
    ev_loop(&ev, sock, test_recv_cb, test_close_cb, test_send_cb, ud);
    ev_send(&ev, sock, (void *)str, strlen(str), 1, NULL);
}
int main(int argc, char *argv[])
{
    MEMCHECK();
    unlimit();
    mutex_init(&muexit);
    cond_init(&condexit);
    sighandle(on_sigcb, NULL);
    tw_ctx tw;
    tw_init(&tw);
    timer_start(&tw.timer);
    tw_add(&tw, 1000, timeout, &tw);

    LOGINIT();    

    CuString *poutput = CuStringNew();
    CuSuite* psuite = CuSuiteNew();

    CuSuiteAddSuite(psuite, test_base());
    CuSuiteAddSuite(psuite, test_utils());

    CuSuiteRun(psuite);
    CuSuiteSummary(psuite, poutput);
    CuSuiteDetails(psuite, poutput);
    printf("%s\n", poutput->buffer);
    
    ev_init(&ev, 2);
    ev_listener(&ev, "0.0.0.0", 15000, acpt_cb, &count);
    mutex_lock(&muexit);
    cond_wait(&condexit, &muexit);
    mutex_unlock(&muexit);

    ev_free(&ev);

    mutex_free(&muexit);
    cond_free(&condexit);
    tw_free(&tw);
    LOGFREE();

    return 0;
}
