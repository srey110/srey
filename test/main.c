#include "test_base.h"
#include "test_utils.h"
#include "lib.h"

#ifdef OS_WIN
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "lib.lib")
#endif

tw_ctx tw;
mutex_ctx muexit;
cond_ctx condexit;
int32_t index = 0;
SOCKET connsock = INVALID_SOCK;
SOCKET acp_connsock = INVALID_SOCK;
static volatile atomic_t count = 0;

static void on_sigcb(int32_t sig, void *arg)
{
    PRINT("catch sign: %d", sig);
    cond_signal(&condexit);
}
static void test_close_cb(ev_ctx *ctx, SOCKET sock, void *ud)
{
    //PRINT("test_close_cb: sock %d ", (int32_t)sock);
    ATOMIC_ADD(&count, -1);
}
static void test_connclose_cb(ev_ctx *ctx, SOCKET sock, void *ud)
{
    ATOMIC_ADD(&count, -1);
    connsock = INVALID_SOCK;
}
static void test_recv_cb(ev_ctx *ctx, SOCKET sock, buffer_ctx *buf, void *ud)
{
    //PRINT("test_recv_cb: sock %d ", (int32_t)sock);
    if (acp_connsock != sock)
    {
        if (randrange(0, 100) <= 1)
        {
            ev_close(ctx, sock);
            //PRINT("test_recv_cb close: sock %d ", (int32_t)sock);
        }
    }
    size_t len = buffer_size(buf);
    char *pk;
    MALLOC(pk, len);
    buffer_remove(buf, pk, len);
    ev_send(ctx, sock, pk, len, 0);
}
static void test_send_cb(ev_ctx *ctx, SOCKET sock, void *data, size_t len, void *ud, int32_t result)
{
    //PRINT("test_send_cb: sock %d  len %d err %d", (int32_t)sock, (int32_t)len, result);
}
static void test_conn_recv_cb(ev_ctx *ctx, SOCKET sock, buffer_ctx *buf, void *ud)
{
    if (buffer_size(buf) <= 2 + sizeof(index))
    {
        return;
    }
    char len[2 + sizeof(index)];
    buffer_copyout(buf, len, sizeof(len));
    u_short pklen = ntohs(*(u_short*)len);
    if (buffer_size(buf) < pklen)
    {
        return;
    }
    int32_t tmp = ntohl(*(u_long*)(len + 2));
    ASSERTAB(tmp == index, "index error.");
    buffer_drain(buf, pklen);
}
static void test_conn_cb(ev_ctx *ctx, int32_t err, SOCKET sock, void *ud)
{
    if (ERR_OK == err)
    {
        connsock = sock;
        ev_loop(ctx, sock, test_conn_recv_cb, test_connclose_cb, test_send_cb, ud);
        ATOMIC_ADD(&count, 1);
    }
}
static void test_acpt_cb(ev_ctx *ctx, SOCKET sock, void *ud)
{
    //PRINT("test_acpt_cb : sock %d ", (int32_t)sock);
    if (INVALID_SOCK == acp_connsock)
    {
        acp_connsock = sock;
    }
    ev_loop(ctx, sock, test_recv_cb, test_close_cb, test_send_cb, ud);
    ATOMIC_ADD(&count, 1);
}
static void timeout(void *arg)
{
    int32_t elapsed = (int32_t)(timer_elapsed(&tw.timer) / (1000 * 1000));
    PRINT("timeout:%d ms link cnt %d", elapsed, ATOMIC_GET(&count));
    if (INVALID_SOCK != connsock)
    {
        //ev_close(arg, connsock);
        char str[4096];
        int32_t len = randrange(100, 4096);
        randstr(str, len);
        u_short total = (u_short)(2 + sizeof(index) + len);
        char *buf;
        MALLOC(buf, total);
        index++;
        total = ntohs(total);
        memcpy(buf, &total, sizeof(total));
        int32_t tmp = ntohl(index);
        memcpy(buf + sizeof(total), &tmp, sizeof(tmp));
        memcpy(buf + sizeof(total) + sizeof(index), str, len);
        ev_send(arg, connsock, buf, 2 + sizeof(index) + len, 0);
    }
    timer_start(&tw.timer);
    tw_add(&tw, 3000, timeout, arg);
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
    
    tw_init(&tw);   
    ev_ctx ev;
    ev_init(&ev, 2);
    ev_listener(&ev, "0.0.0.0", 15000, test_acpt_cb, NULL);
    ev_connecter(&ev, "127.0.0.1", 15000, test_conn_cb, NULL);

    timer_start(&tw.timer);
    tw_add(&tw, 3000, timeout, &ev);

    mutex_lock(&muexit);
    cond_wait(&condexit, &muexit);
    mutex_unlock(&muexit);
    if (INVALID_SOCK != connsock)
    {
        ev_close(&ev, connsock);
        MSLEEP(100);
    }
    PRINT("link cnt %d", ATOMIC_GET(&count));
    ev_free(&ev);

    mutex_free(&muexit);
    cond_free(&condexit);
    tw_free(&tw);
    LOGFREE();

    return 0;
}
