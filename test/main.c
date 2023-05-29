#include "test_base.h"
#include "test_utils.h"
#include "lib.h"

#ifdef OS_WIN
#include "vld.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#endif

int main(int argc, char *argv[]) {
    MEMCHECK();
    unlimit();
    srand((unsigned int)time(NULL)); 
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
    LOGFREE();
    return 0;
}
