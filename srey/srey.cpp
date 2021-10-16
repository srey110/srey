#include "lib.h"

using namespace SREY_NS;

#ifdef OS_WIN
#include "../vld/vld.h"
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#pragma comment(lib, "vld.lib")
#endif

class AB
{
public:
    AB()
    {
        paaa = new int;
    };
    ~AB()
    {
        printf("111111111111111\n");
        delete paaa;
    };

private:
    int *paaa;
};

struct A
{
    int aa;
    char ac[100];
    AB abcx;
};
struct B : A
{
    int bbb;
    char *p;
    void c()
    {
        int i = 0;
    }
};

int main(int argc, char *argv[])
{
    A *p = new B();
    delete p;
    cchan chanscok(100);
    ciocp iocp;  
    SOCKET acpair[2];  
    iocp.start();

    //   ::/0  ::1
    iocp.listener(&chanscok, "0.0.0.0", 15000, true);
    /*iocp.listener(&chanscok, "0.0.0.0", 15001, false);
    iocp.listener(&chanscok, "fe80::c95e:3ff8:a284:fe13%17", 15002, true);
    iocp.listener(&chanscok, "0.0.0.0", 15003, true);

    iocp.connectter(&chanscok, "fe80::c95e:3ff8:a284:fe13%17", 15002, false);
    iocp.connectter(&chanscok, "127.0.0.1", 15003);

    sockpair(acpair);

    iocp.addsock(&chanscok, acpair[0]);
    iocp.addsock(&chanscok, acpair[1]);

    cnetaddr addrss;
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    addrss.setaddr("0.0.0.0", 0);
    int32_t irtn = bind(sock, addrss.getaddr(), addrss.getsize());
    if (ERR_OK != irtn)
    {
        PRINTF("%s", ERRORSTR(ERRNO));
    }
    iocp.addsock(&chanscok, sock);

    sock = socket(AF_INET6, SOCK_DGRAM, 0);
    addrss.setaddr("fe80::c95e:3ff8:a284:fe13%17", 0);
    irtn = bind(sock, addrss.getaddr(), addrss.getsize());
    if (ERR_OK != irtn)
    {
        PRINTF("%s", ERRORSTR(ERRNO));
    }
    iocp.addsock(&chanscok, sock);*/
    //piocpev->stop();

    struct event *pev;
    while (true)
    {
        if (!chanscok.recvt(&pev))
        {
            continue;
        }

        switch (pev->evtype)
        {
            break;
        case EV_ACCEPT:
            {
                struct sockctx *pctx = (struct sockctx *)pev->data;
                printf("accept socket %d\n", (int32_t)pctx->sock);
            }
            break;
        case EV_CONNECT:
            break;
        case EV_CLOSE:
            break;
        case EV_READ:
            break;
        case EV_WRITE:
            break;
        }
        SAFE_DEL(pev);
    }

    return 0;
    ctimeev timeev;
    ctimer objtimer;
    cthread thread;
    thread.creat(&timeev);
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
            timeev.addtimer(&chan, rand() % (10 * 1000) + 10, NULL);
            uladd++;
        }

        MSLEEP(10);
        ullast += 10;
    }

    return 0;
}
