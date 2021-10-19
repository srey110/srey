#include "lib.h"

#ifdef OS_WIN
#include "../vld/vld.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#pragma comment(lib, "vld.lib")
#endif

void timewheel(void *p1, void*p2, void*p3)
{
    struct wot_ctx *ptw = p1;
    struct timer_ctx *ptimer = p2;
    u_long uicur = 0;
    while (1)
    {
        uicur = (u_long)(timer_nanosec(ptimer) / (1000 * 1000));
        wot_run(ptw, uicur);
    }
};
int main(int argc, char *argv[])
{
    struct timer_ctx timer;
    timer_init(&timer);
    struct chan_ctx chan;
    chan_init(&chan, 100);
    struct wot_ctx tw;
    wot_init(&tw, (u_long)(timer_nanosec(&timer) / (1000 * 1000)));
    struct thread_ctx th1;
    thread_init(&th1);
    thread_creat(&th1, timewheel, &tw, &timer, NULL);

    u_long ultime;
    u_long ullast = 0;
    uint64_t uladd = 0;
    uint64_t ulcount = 0;
    uint64_t uidiff = 0;
    uint32_t uitmp;
    struct twnode *node;
    struct ev_ctx *pev;
    void *ptmp;
    while (1)
    {
        if (ERR_OK == chan_canrecv(&chan))
        {
            ptmp = NULL;
            if (ERR_OK == chan_recv(&chan, &ptmp))
            {
                pev = (ev_ctx *)ptmp;
                node = UPCAST(pev, struct twnode, ev);
                ultime = (uint32_t)((timer_nanosec(&timer) / (1000 * 1000)));
                uitmp = (uint32_t)(ultime - node->expires);
                uidiff += uitmp;
                ulcount++;
                printf("total:%d  time out:%d  diff:%d  per:%d\n", (uint32_t)uladd, (uint32_t)ulcount, uitmp, (uint32_t)((double)uidiff / ulcount));
                SAFE_FREE(node);
            }
        }
        if (ullast % 100 == 0)
        {
            wot_add(&tw, &chan, (u_long)(timer_nanosec(&timer) / (1000 * 1000)), rand() % (10 * 1000) + 10, NULL);
            uladd++;
        }

        MSLEEP(10);
        ullast += 10;
    }

    thread_join(&th1);
    //const char *perr = _fmterror(10022);
    return 0;
}
