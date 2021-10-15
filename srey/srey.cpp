#include "lib.h"

using namespace SREY_NS;

#ifdef OS_WIN
#include "../vld/vld.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#pragma comment(lib, "vld.lib")
#endif

int main(int argc, char *argv[])
{
    cev ev;
    ctimer objtimer;
    cthread thread;
    thread.creat(&ev);
    thread.waitstart();

    srand((uint32_t)objtimer.nanosec() / 1000);

    cchan chan(ONEK);
    u_long ultime;
    u_long ullast = INIT_NUMBER;
    uint64_t uladd = INIT_NUMBER;
    uint64_t ulcount = INIT_NUMBER;
    uint64_t uidiff = INIT_NUMBER;
    uint32_t uitmp;
    struct twnode *node;
    while (true)
    {
        if (chan.canrecv())
        {
            if (chan.recvt(&node))
            {
                ultime = (uint32_t)(objtimer.nanosec() / (1000 * 1000));
                uitmp = (uint32_t)(ultime - node->expires);
                uidiff += uitmp;
                ulcount++;
                printf("total:%d  time out:%d  diff:%d  per:%d\n", (uint32_t)uladd, (uint32_t)ulcount, uitmp, (uint32_t)((double)uidiff / ulcount));
                SAFE_DEL(node);
            }
        }

        if (ullast % 100 == INIT_NUMBER)
        {
            ev.addtimer(&chan, rand() % (10 * 1000) + 10, NULL);
            uladd++;
        }

        MSLEEP(10);
        ullast += 10;
    }

    return 0;
}
