#include "utils.h"
#include "timer.h"
#include "thread.h"
#include "chan.h"
#include "snowflake.h"

using namespace SREY_NS;

#ifdef OS_WIN
#include "../vld/vld.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#pragma comment(lib, "vld.lib")
#endif

int main(int argc, char *argv[])
{
    ctimer objtimer;
    objtimer.start();
    USLEEP(1000);
    uint64_t uielapsed = objtimer.elapsed();
    PRINTF("%I64d", uielapsed);

    csnowflake objsfid(5, 8);
    objtimer.start();
    for (int32_t i = 0; i < 200000; i++)
    {
        objsfid.id();
    }
    uielapsed = objtimer.elapsed();
    PRINTF("%I64d", uielapsed);

    return 0;
}
