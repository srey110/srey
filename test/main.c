#include "test_base.h"
#include "test_utils.h"
#include "lib.h"

#ifdef OS_WIN
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#endif

mutex_ctx muexit;
cond_ctx condexit;

void on_sigcb(int32_t sig, void *arg)
{
    PRINT("catch sign: %d", sig);
    cond_signal(&condexit);
}
int main(int argc, char *argv[])
{
    MEMCHECK();
    unlimit();
    mutex_init(&muexit);
    cond_init(&condexit);
    sighandle(on_sigcb, NULL);

    LOGINIT();

    CuString *poutput = CuStringNew();
    CuSuite* psuite = CuSuiteNew();

    CuSuiteAddSuite(psuite, test_base());
    CuSuiteAddSuite(psuite, test_utils());

    CuSuiteRun(psuite);
    CuSuiteSummary(psuite, poutput);
    CuSuiteDetails(psuite, poutput);
    printf("%s\n", poutput->buffer);

    mutex_lock(&muexit);
    cond_wait(&condexit, &muexit);
    mutex_unlock(&muexit);

    mutex_free(&muexit);
    cond_free(&condexit);
    LOGFREE();

    return 0;
}
