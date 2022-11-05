#include "test_base.h"
#include "test_utils.h"
#include "lib.h"

#ifdef OS_WIN
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#endif

mutex_ctx muexit;
cond_ctx condexit;
static uint32_t count = 0;

void on_sigcb(int32_t sig, void *arg)
{
    PRINT("catch sign: %d", sig);
    cond_signal(&condexit);
}
void timeout(void *arg)
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

    ev_ctx ev;
    ev_init(&ev, 2);

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
