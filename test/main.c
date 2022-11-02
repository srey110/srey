#include "test_base.h"
#include "test_utils.h"
#include "lib.h"

#ifdef OS_WIN
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#endif

int main(int argc, char *argv[])
{
    MEMCHECK();
    LOGINIT();
    CuString *poutput = CuStringNew();
    CuSuite* psuite = CuSuiteNew();

    CuSuiteAddSuite(psuite, test_base());
    CuSuiteAddSuite(psuite, test_utils());

    CuSuiteRun(psuite);
    CuSuiteSummary(psuite, poutput);
    CuSuiteDetails(psuite, poutput);
    printf("%s\n", poutput->buffer);
    LOGFREE();
    return 0;
}
