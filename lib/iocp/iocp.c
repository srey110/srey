#include "overlap.h"
#include "netapi.h"
#include "thread.h"
#include "netutils.h"
#include "loger.h"
#include "utils.h"

#if defined(OS_WIN)

#define NOTIFI_EXIT_KEY  ((ULONG_PTR)-1)

static void *_getexfunc(SOCKET fd, GUID  *pguid)
{
    void *pfunc = NULL;
    DWORD dbytes = 0;
    int32_t irtn = WSAIoctl(fd,
        SIO_GET_EXTENSION_FUNCTION_POINTER,
        pguid,
        sizeof(GUID),
        &pfunc,
        sizeof(pfunc),
        &dbytes,
        NULL,
        NULL);
    ASSERTAB(irtn != SOCKET_ERROR, ERRORSTR(ERRNO));

    return pfunc;
}
static void _initexfunc(struct netev_ctx *pctx)
{
    GUID accept_uid = WSAID_ACCEPTEX;
    GUID connect_uid = WSAID_CONNECTEX;
    GUID acceptaddrs_uid = WSAID_GETACCEPTEXSOCKADDRS;
    GUID disconnect_uid = WSAID_DISCONNECTEX;

    SOCKET sock = WSASocket(AF_INET,
        SOCK_STREAM,
        0,
        NULL,
        0,
        0);
    ASSERTAB(INVALID_SOCK != sock, ERRORSTR(ERRNO));
    pctx->acceptex = _getexfunc(sock, &accept_uid);
    pctx->connectex = _getexfunc(sock, &connect_uid);
    pctx->acceptaddrsex = _getexfunc(sock, &acceptaddrs_uid);
    pctx->disconnectex = _getexfunc(sock, &disconnect_uid);
    SOCK_CLOSE(sock);
}
struct netev_ctx *netev_new()
{
    WSADATA wsdata;
    WORD ver = MAKEWORD(2, 2);
    ASSERTAB(ERR_OK == WSAStartup(ver, &wsdata), ERRORSTR(ERRNO));

    struct netev_ctx *pctx = MALLOC(sizeof(struct netev_ctx));
    ASSERTAB(NULL != pctx, ERRSTR_MEMORY);
    pctx->thcnt = (int32_t)procscnt() * 2;
    pctx->thiocp = MALLOC(sizeof(struct thread_ctx) * pctx->thcnt);
    ASSERTAB(NULL != pctx->thiocp, ERRSTR_MEMORY);
    for (int32_t i = 0; i < pctx->thcnt; i++)
    {
        thread_init(&pctx->thiocp[i]);
    }
    _initexfunc(pctx);
    pctx->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE,
        NULL,
        0,
        pctx->thcnt);
    ASSERTAB(NULL != pctx->iocp, ERRORSTR(ERRNO));

    return pctx;
}
void netev_free(struct netev_ctx *pctx)
{
    int32_t i;
    for (i = 0; i < pctx->thcnt; i++)
    {
        if (!PostQueuedCompletionStatus(pctx->iocp,
            0,
            NOTIFI_EXIT_KEY,
            NULL))
        {
            int32_t irtn = ERRNO;
            LOG_ERROR("error code:%d message:%s", irtn, ERRORSTR(irtn));
        }
    }
    for (i = 0; i < pctx->thcnt; i++)
    {
        thread_join(&pctx->thiocp[i]);
    }

    (void)CloseHandle(pctx->iocp);
    FREE(pctx->thiocp);
    (void)WSACleanup();
    FREE(pctx);
}
static void _icop_loop(void *p1, void *p2, void *p3)
{
    BOOL brtn;
    DWORD dbytes;
    ULONG_PTR ulkey;
    int32_t ierr;
    OVERLAPPED *poverlap;
    struct netev_ctx *pctx = (struct netev_ctx *)p1;
    for (;;)
    {
        ierr = ERR_OK;
        brtn = GetQueuedCompletionStatus(pctx->iocp,
            &dbytes,
            &ulkey,
            &poverlap,
            INFINITE);
        if (NOTIFI_EXIT_KEY == ulkey)
        {
            break;
        }
        if (!brtn)
        {
            ierr = ERRNO;
            if (WAIT_TIMEOUT == ierr)
            {
                continue;
            }
        }
        if (NULL == poverlap)
        {
            continue;
        }
        struct overlap_ctx *polctx = UPCAST(poverlap, struct overlap_ctx, overlapped);
        polctx->overlap_cb(pctx, polctx, (uint32_t)dbytes, ierr);
    }
}
void netev_loop(struct netev_ctx *pctx)
{
    for (int32_t i = 0; i < pctx->thcnt; i++)
    {
        thread_creat(&pctx->thiocp[i], _icop_loop, pctx, NULL, NULL);
    }
}

#endif // OS_WIN
