#include "iocp/iocp.h"
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
    SAFE_CLOSESOCK(sock);
}
void netev_init(struct netev_ctx *pctx)
{
    WSADATA wsdata;
    WORD ver = MAKEWORD(2, 2);
    ASSERTAB(ERR_OK == WSAStartup(ver, &wsdata), ERRORSTR(ERRNO));

    pctx->threadcnt = (int32_t)procscnt() * 2;
    pctx->thread = MALLOC(sizeof(struct thread_ctx) * pctx->threadcnt);
    ASSERTAB(NULL != pctx->thread, ERRSTR_MEMORY);
    for (int32_t i = 0; i < pctx->threadcnt; i++)
    {
        thread_init(&pctx->thread[i]);
    }
    _initexfunc(pctx);
    pctx->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE,
        NULL,
        0,
        pctx->threadcnt);
    ASSERTAB(NULL != pctx->iocp, ERRORSTR(ERRNO));
}
void netev_free(struct netev_ctx *pctx)
{
    int32_t i;
    for (i = 0; i < pctx->threadcnt; i++)
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
    for (i = 0; i < pctx->threadcnt; i++)
    {
        thread_join(&pctx->thread[i]);
    }

    (void)CloseHandle(pctx->iocp);
    SAFE_FREE(pctx->thread);
    (void)WSACleanup();
}
static void _icop_loop(void *p1, void *p2, void *p3)
{
    BOOL brtn;
    DWORD dbytes;
    ULONG_PTR ulkey;
    int32_t ierr;
    OVERLAPPED *poverlap;
    struct netev_ctx *pctx = (struct netev_ctx *)p1;
    while (1)
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
        polctx->overlap_cb(pctx, polctx, dbytes, ierr);
    }
}
void netev_loop(struct netev_ctx *pctx)
{
    for (int32_t i = 0; i < pctx->threadcnt; i++)
    {
        thread_creat(&pctx->thread[i], _icop_loop, pctx, NULL, NULL);
    }
}

#endif // OS_WIN
