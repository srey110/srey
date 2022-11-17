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
int32_t pk_index = 0;
SOCKET connsock = INVALID_SOCK;
static volatile atomic_t count = 0;

static void on_sigcb(int32_t sig, void *arg)
{
    PRINT("catch sign: %d", sig);
    cond_signal(&condexit);
}
static void test_close_cb(ev_ctx *ctx, SOCKET sock, ud_cxt *ud)
{
    //PRINT("test_close_cb: sock %d ", (int32_t)sock);
    ATOMIC_ADD(&count, -1);
}
static void test_connclose_cb(ev_ctx *ctx, SOCKET sock, ud_cxt *ud)
{
    //PRINT("test_connclose_cb: sock %d ", (int32_t)sock);
    connsock = INVALID_SOCK;
}
static void test_recv_cb(ev_ctx *ctx, SOCKET sock, buffer_ctx *buf, size_t lens, ud_cxt *ud)
{
    //PRINT("test_recv_cb: sock %d ", (int32_t)sock);
    //if (sock % randrange(1, 100) == 0)
    //{
    //    ev_close(ctx, sock);
    //    //PRINT("close socket: sock %d ", (int32_t)sock);
    //    return;
    //}
    size_t len = buffer_size(buf);
    
    char *pk;
    MALLOC(pk, len);
    buffer_remove(buf, pk, len);
    ev_send(ctx, sock, pk, len, 0);

    //buffer_drain(buf,len);
}
static void test_send_cb(ev_ctx *ctx, SOCKET sock, size_t len, ud_cxt *ud)
{
    //PRINT("test_send_cb: sock %d  len %d ", (int32_t)sock, (int32_t)len);
}
static void test_conn_recv_cb(ev_ctx *ctx, SOCKET sock, buffer_ctx *buf, size_t lens, ud_cxt *ud)
{
    //PRINT("test_conn_recv_cb: sock %d", (int32_t)sock);
    if (buffer_size(buf) <= 2 + sizeof(pk_index))
    {
        return;
    }
    char len[2 + sizeof(pk_index)];
    buffer_copyout(buf, len, sizeof(len));
    u_short pklen = ntohs(*(u_short*)len);
    if (buffer_size(buf) < pklen)
    {
        return;
    }
    int32_t tmp = (int32_t)(ntohl(*(u_long*)(len + 2)));
    if (tmp != pk_index)
    {
        PRINT("index error.recv: %d  cur %d", tmp, pk_index);
    }
    buffer_drain(buf, buffer_size(buf));
}
static int32_t test_conn_cb(ev_ctx *ctx, SOCKET sock, ud_cxt *ud)
{
    if (INVALID_SOCK != sock)
    {
        //PRINT("%s", "connect ok.");
        connsock = sock;
    }
    else
    {
        //PRINT("%s", "connect error.");
    }
    return ERR_OK;
}
static int32_t test_acpt_cb(ev_ctx *ctx, SOCKET sock, ud_cxt *ud)
{
    //PRINT("test_acpt_cb : sock %d ", (int32_t)sock);
    ATOMIC_ADD(&count, 1);
    return ERR_OK;
}
static void timeout(void *arg)
{
    int32_t elapsed = (int32_t)timer_elapsed_ms(&tw.timer);
    PRINT("timeout:%d ms link cnt %d", elapsed, ATOMIC_GET(&count));
    if (INVALID_SOCK != connsock)
    {
        /*ev_close(arg, connsock);
        connsock = INVALID_SOCK;*/
        char str[100];
        int32_t len = randrange(1, sizeof(str));
        ASSERTAB(sizeof(str) > len, "randrange error.");
        randstr(str, len);
        u_short total = (u_short)(2 + sizeof(pk_index) + len);
        char *buf;
        MALLOC(buf, total);
        pk_index++;
        //PRINT("send pack index: %d", pk_index);
        total = ntohs(total);
        memcpy(buf, &total, sizeof(total));
        int32_t tmp = ntohl(pk_index);
        memcpy(buf + sizeof(total), &tmp, sizeof(tmp));
        memcpy(buf + sizeof(total) + sizeof(pk_index), str, len);
        ev_send(arg, connsock, buf, 2 + sizeof(pk_index) + len, 0);
    }
    else
    {
        cbs_ctx cbs;
        ZERO(&cbs, sizeof(cbs));
        cbs.conn_cb = test_conn_cb;
        cbs.c_cb = test_connclose_cb;
        cbs.r_cb = test_conn_recv_cb;
        cbs.s_cb = test_send_cb;
        ev_connect(arg, "127.0.0.1", 15000, &cbs, NULL);
    }
    timer_start(&tw.timer);
    tw_add(&tw, 5000, timeout, arg);
}
void testtt(void *arg)
{
    MSLEEP(1000);
}
int main(int argc, char *argv[])
{
    pthread_t aaa = thread_creat(testtt, NULL);
    thread_join(aaa);

    MEMCHECK();
    unlimit();
    srand((unsigned int)time(NULL));
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
    cbs_ctx cbs;
    ZERO(&cbs, sizeof(cbs));
    cbs.acp_cb = test_acpt_cb;
    cbs.c_cb = test_close_cb;
    cbs.r_cb = test_recv_cb;
    cbs.s_cb = test_send_cb;
    ev_listen(&ev, "0.0.0.0", 15000, &cbs, NULL);

    timer_start(&tw.timer);
    tw_add(&tw, 5000, timeout, &ev);

    mutex_lock(&muexit);
    cond_wait(&condexit, &muexit);
    mutex_unlock(&muexit);
    if (INVALID_SOCK != connsock)
    {
        ev_close(&ev, connsock);
        MSLEEP(500);
    }
    PRINT("link cnt %d", ATOMIC_GET(&count));
    ev_free(&ev);

    mutex_free(&muexit);
    cond_free(&condexit);
    tw_free(&tw);
    LOGFREE();

    return 0;
}
