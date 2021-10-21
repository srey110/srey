#include "lib.h"

#ifdef OS_WIN
#include "../vld/vld.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#pragma comment(lib, "vld.lib")
#endif

u_long curtick(struct timer_ctx *ptimer)
{
    return (u_long)(timer_nanosec(ptimer) / (1000 * 1000 * 10));
}
void timewheel(void *p1, void*p2, void*p3)
{
    struct wot_ctx *ptw = p1;
    struct timer_ctx *ptimer = p2;
    u_long uicur = 0;
    while (1)
    {
        uicur = curtick(ptimer);
        wot_run(ptw, uicur);
    }
};
int main(int argc, char *argv[])
{
    LOGINIT();
    MSLEEP(100);
    LOG_DEBUG("%s", "11111111111111111");
    LOG_INFO("%s", "2222222222222222222");
    LOG_ERROR("%s", "33333333333333333333");
    LOG_FATAL("%s", "44444444444444444444");
    MSLEEP(200);
    LOGFREE();

    //return 0;

    struct timer_ctx timer;
    timer_init(&timer);
    struct chan_ctx chan;
    chan_init(&chan, 100);
    struct wot_ctx tw;
    wot_init(&tw, curtick(&timer));
    struct thread_ctx th1;
    thread_init(&th1);
    thread_creat(&th1, timewheel, &tw, &timer, NULL);

    u_long ultime;
    uint64_t ullast = 0;
    uint64_t uladd = 0;
    uint64_t ulcount = 0;
    uint64_t uidiff = 0;
    uint32_t uitmp;
    struct twnode_ctx *node;
    struct ev_ctx *pev;
    void *ptmp;
    wot_add(&tw, &chan, curtick(&timer), 100, NULL);
    while (1)
    {
        if (ERR_OK == chan_canrecv(&chan))
        {
            ptmp = NULL;
            if (ERR_OK == chan_recv(&chan, &ptmp))
            {
                pev = (ev_ctx *)ptmp;
                node = UPCAST(pev, struct twnode_ctx, ev);
                ultime = (uint32_t)curtick(&timer);
                uitmp = (uint32_t)(ultime - node->expires);
                uidiff += uitmp;
                ulcount++;
                printf("total:%d  time out:%d  diff:%d  per:%d\n", 
                    (uint32_t)uladd, (uint32_t)ulcount, uitmp, (uint32_t)((double)uidiff / ulcount));
                wot_add(&tw, &chan, curtick(&timer), 100, NULL);
            }
        }
        if (ullast % 100 == 0)
        {
            //wot_add(&tw, &chan, curtick(&timer), rand() % (1000 * 1000) + 10, NULL, 0);
            uladd++;
        }

        MSLEEP(10);
        ullast += 10;
    }

    thread_join(&th1);
    //const char *perr = _fmterror(10022);
    return 0;
}
