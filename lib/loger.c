#include "loger.h"
#include "utils.h"

#ifndef OS_WIN
#define CLR_CLR         "\033[0m"       /* »Ö¸´ÑÕÉ« */
#define CLR_BLACK       "\033[30m"      /* ºÚÉ«×Ö */
#define CLR_RED         "\033[31m"      /* ºìÉ«×Ö */
#define CLR_GREEN       "\033[32m"      /* ÂÌÉ«×Ö */
#define CLR_YELLOW      "\033[33m"      /* »ÆÉ«×Ö */
#define CLR_BLUE        "\033[34m"      /* À¶É«×Ö */
#define CLR_PURPLE      "\033[35m"      /* ×ÏÉ«×Ö */
#define CLR_SKYBLUE     "\033[36m"      /* ÌìÀ¶×Ö */
#define CLR_WHITE       "\033[37m"      /* °×É«×Ö */

#define CLR_BLK_WHT     "\033[40;37m"   /* ºÚµ×°××Ö */
#define CLR_RED_WHT     "\033[41;37m"   /* ºìµ×°××Ö */
#define CLR_GREEN_WHT   "\033[42;37m"   /* ÂÌµ×°××Ö */
#define CLR_YELLOW_WHT  "\033[43;37m"   /* »Æµ×°××Ö */
#define CLR_BLUE_WHT    "\033[44;37m"   /* À¶µ×°××Ö */
#define CLR_PURPLE_WHT  "\033[45;37m"   /* ×Ïµ×°××Ö */
#define CLR_SKYBLUE_WHT "\033[46;37m"   /* ÌìÀ¶µ×°××Ö */
#define CLR_WHT_BLK     "\033[47;30m"   /* °×µ×ºÚ×Ö */
#endif

typedef struct loginfo_ctx
{
    LOG_LEVEL lv;
    char *plog;
    char time[TIME_LENS];
}loginfo_ctx;
typedef struct logworker_ctx
{
    FILE *file;
#if defined(OS_WIN)
    HANDLE hout;
    CONSOLE_SCREEN_BUFFER_INFO stcsbi;
#endif
    char date[TIME_LENS];
    char filename[PATH_LENS];
    char *path;
    struct loger_ctx *ploger;
}logworker_ctx;
struct loger_ctx g_logerctx;

const char *_getlvstr(const LOG_LEVEL emlv)
{
    switch (emlv)
    {
    case LOGLV_FATAL:
        return "FATAL";
    case LOGLV_ERROR:
        return "ERROR";
    case LOGLV_WARN:
        return " WARN";
    case LOGLV_INFO:
        return " INFO";
    case LOGLV_DEBUG:
        return "DEBUG";
    default:
        break;
    }

    return "UNKNOWN";
}
static void _worker_init(struct logworker_ctx *pctx)
{
    pctx->path = NULL;
    pctx->file = NULL;
    ZERO(pctx->filename, sizeof(pctx->filename));

    //´´½¨ÎÄ¼þ¼Ð
    char acpath[PATH_LENS];
    ASSERTAB(ERR_OK == getprocpath(acpath), "getpath failed.");
    size_t ilens = strlen(acpath) + strlen(PATH_SEPARATORSTR) + strlen("logs") + 1;
    pctx->path = CALLOC(ilens, sizeof(char));
    ASSERTAB(NULL != pctx->path, ERRSTR_MEMORY);
    SNPRINTF(pctx->path, ilens - 1, "%s%s%s", acpath, PATH_SEPARATORSTR, "logs");
    PRINTF("log path: %s.", pctx->path);

    if (ERR_OK != ACCESS(pctx->path, 0))
    {
        if (ERR_OK != MKDIR(pctx->path))
        {
            PRINTF("mkdir(%s) failed.", pctx->path);
            SAFE_FREE(pctx->path);
        }
    }
#if defined(OS_WIN)
    //»ñÈ¡±ê×¼Êä³ö
    pctx->hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (INVALID_HANDLE_VALUE == pctx->hout
        || NULL == pctx->hout)
    {
        pctx->hout = NULL;
        PRINTF("%s", "GetStdHandle(STD_OUTPUT_HANDLE) error.");
        return;
    }
    if (!GetConsoleScreenBufferInfo(pctx->hout, &pctx->stcsbi))
    {
        PRINTF("%s", "GetConsoleScreenBufferInfo error.");
        return;
    }
#endif  
}
static void _worker_free(struct logworker_ctx *pctx)
{
    if (NULL != pctx->file)
    {
        fclose(pctx->file);
        pctx->file = NULL;
    }
    SAFE_FREE(pctx->path);
}
static inline int32_t _worker_getfile(struct logworker_ctx *pctx)
{
    if (NULL == pctx->path)
    {
        return ERR_FAILED;
    }
    //ÅÐ¶ÏÈÕÆÚÊÇ·ñ¸ü¸Ä£¬¸ü¸ÄÔò¸ü»»logÎÄ¼þÃû
    nowtime("%Y%m%d", pctx->date);
    if (0 != strcmp(pctx->date, pctx->filename))
    {
        if (NULL != pctx->file)
        {
            fclose(pctx->file);
            pctx->file = NULL;
        }

        size_t ilens = strlen(pctx->date);
        memcpy(pctx->filename, pctx->date, ilens);
        pctx->filename[ilens] = '\0';
    }
    //´ò¿ªÎÄ¼þ
    if (NULL == pctx->file)
    {
        char actmp[PATH_LENS] = { 0 };
        SNPRINTF(actmp, sizeof(actmp) - 1, "%s%s%s%s", pctx->path, PATH_SEPARATORSTR, pctx->filename, ".log");
        pctx->file = fopen(actmp, "a");
        if (NULL == pctx->file)
        {
            PRINTF("fopen(%s, a) error.", actmp);
            return ERR_FAILED;
        }
    }

    return ERR_OK;
}
static inline void _worker_writelog(struct logworker_ctx *pctx, struct loginfo_ctx *pinfo)
{
    const char *pn = "\n";
    (void)fwrite(pinfo->time, 1, strlen(pinfo->time), pctx->file);
    (void)fwrite(pinfo->plog, 1, strlen(pinfo->plog), pctx->file);
    (void)fwrite(pn, 1, strlen(pn), pctx->file);
}
static inline void _worker_printlog(struct logworker_ctx *pctx, struct loginfo_ctx *pinfo)
{
    if (0 == ATOMIC_GET(&pctx->ploger->print))
    {
        return;
    }
#if defined(OS_WIN)
    if (NULL == pctx->hout)
    {
        return;
    }
    switch (pinfo->lv)
    {
    case LOGLV_FATAL:
        SetConsoleTextAttribute(pctx->hout, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | BACKGROUND_RED);
        break;
    case LOGLV_ERROR:
        SetConsoleTextAttribute(pctx->hout, FOREGROUND_RED);
        break;
    case LOGLV_WARN:
        SetConsoleTextAttribute(pctx->hout, FOREGROUND_RED | FOREGROUND_GREEN);
        break;
    case LOGLV_INFO:
        SetConsoleTextAttribute(pctx->hout, FOREGROUND_GREEN);
        break;
    case LOGLV_DEBUG:
        SetConsoleTextAttribute(pctx->hout, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        break;
    default:
        break;
    }
    printf("%s%s\n", pinfo->time, pinfo->plog);
    SetConsoleTextAttribute(pctx->hout, pctx->stcsbi.wAttributes);
#else
    switch (pinfo->lv)
    {
    case LOGLV_FATAL:
        printf("%s%s%s%s\n", CLR_RED_WHT, pinfo->time, pinfo->plog, CLR_CLR);
        break;
    case LOGLV_ERROR:
        printf("%s%s%s%s\n", CLR_RED, pinfo->time, pinfo->plog, CLR_CLR);
        break;
    case LOGLV_WARN:
        printf("%s%s%s%s\n", CLR_YELLOW, pinfo->time, pinfo->plog, CLR_CLR);
        break;
    case LOGLV_INFO:
        printf("%s%s%s%s\n", CLR_GREEN, pinfo->time, pinfo->plog, CLR_CLR);
        break;
    case LOGLV_DEBUG:
        printf("%s%s%s%s\n", CLR_WHITE, pinfo->time, pinfo->plog, CLR_CLR);
        break;
    }
#endif
}
static inline void _worker_freeloginfo(loginfo_ctx *pinfo)
{
    if (NULL != pinfo)
    {
        SAFE_FREE(pinfo->plog);
        SAFE_FREE(pinfo);
    }
}
static void _loger(void *pparam, void *p2, void *p3)
{
    struct logworker_ctx stworker;
    stworker.ploger = (struct loger_ctx *)pparam;
    _worker_init(&stworker);

    void *pinfo;
    while (0 == ATOMIC_GET(&stworker.ploger->stop))
    {
        pinfo = NULL;
        if (ERR_OK != chan_recv(&stworker.ploger->chan, &pinfo))
        {
            _worker_freeloginfo((loginfo_ctx *)pinfo);
            continue;
        }

        _worker_printlog(&stworker, (loginfo_ctx *)pinfo);
        if (ERR_OK != _worker_getfile(&stworker))
        {
            _worker_freeloginfo((loginfo_ctx *)pinfo);
            continue;
        }

        _worker_writelog(&stworker, (loginfo_ctx *)pinfo);
        _worker_freeloginfo((loginfo_ctx *)pinfo);
    }

    _worker_free(&stworker);
}

void loger_init(struct loger_ctx *pctx)
{
    pctx->stop = 0;
    pctx->lv = LOGLV_DEBUG;
    pctx->print = 1;
    chan_init(&pctx->chan, ONEK);
    thread_init(&pctx->thread);
    thread_creat(&pctx->thread, _loger, pctx, NULL, NULL);
}
void loger_free(struct loger_ctx *pctx)
{
    while (chan_size(&pctx->chan) > 0);
    ATOMIC_SET(&pctx->stop, 1);
    chan_close(&pctx->chan);
    thread_join(&pctx->thread);
    chan_free(&pctx->chan);
}
void loger_setlv(struct loger_ctx *pctx, const LOG_LEVEL emlv)
{
    ATOMIC_SET(&pctx->lv, emlv);
}
void loger_setprint(struct loger_ctx *pctx, const int32_t iprint)
{
    ATOMIC_SET(&pctx->print, iprint);
}
static inline void _nowmtime(char atime[TIME_LENS])
{
    struct timeval tv;
    timeofday(&tv);
    time_t t = tv.tv_sec;
    ZERO(atime, TIME_LENS);
    strftime(atime, TIME_LENS - 1, "[%H:%M:%S", localtime(&t));
    size_t uilen = strlen(atime);
    SNPRINTF(atime + uilen, TIME_LENS - uilen - 1, " %03d]", (int32_t)(tv.tv_usec / 1000));
}
void loger_log(struct loger_ctx *pctx, const LOG_LEVEL emlv, const char *pformat, ...)
{
    if (emlv > (int32_t)ATOMIC_GET(&pctx->lv))
    {
        return;
    }
    struct loginfo_ctx *pinfo = MALLOC(sizeof(loginfo_ctx));
    if (NULL == pinfo)
    {
        PRINTF("%s", ERRSTR_MEMORY);
        return;
    }

    pinfo->lv = emlv;
    _nowmtime(pinfo->time);

    va_list va;
    va_start(va, pformat);
    pinfo->plog = formatargs(pformat, va);
    va_end(va);

    if (ERR_OK != chan_send(&pctx->chan, (void*)pinfo))
    {
        PRINTF("write log error. %s", pinfo->plog);
        SAFE_FREE(pinfo->plog);
        SAFE_FREE(pinfo);
    }
}
