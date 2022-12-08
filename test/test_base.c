#include "test_base.h"
#include "lib.h"

void test_memory(CuTest* tc)
{
    int *pTest;
    MALLOC(pTest, sizeof(int));
    FREE(pTest);

    char *pBuf;
    CALLOC(pBuf, (size_t)5, sizeof(int));

    char *pNew;
    REALLOC(pNew, pBuf, (size_t)40);    
    FREE(pNew);
}
void test_base(CuSuite* suite)
{
    SUITE_ADD_TEST(suite, test_memory);
}
