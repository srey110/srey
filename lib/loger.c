#include "loger.h"
#include "utils.h"

loger_ctx g_logerctx;

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

#define LOG_FOLDER  "logs"

typedef struct logworker_ctx {
    FILE *file;
#if defined(OS_WIN)
    HANDLE hout;
    CONSOLE_SCREEN_BUFFER_INFO stcsbi;
#endif
    loger_ctx *ploger;
    char lognam[TIME_LENS];
    char path[PATH_LENS];
}logworker_ctx;
typedef struct loginfo_ctx {
    LOG_LEVEL lv;
    char *data;
    char time[TIME_LENS];
}loginfo_ctx;

const char *_getlvstr(const LOG_LEVEL emlv) {
    switch (emlv) {
    case LOGLV_FATAL:
        return "FATAL";
    case LOGLV_ERROR:
        return "ERROR";
    case LOGLV_WARN:
        return "WARN";
    case LOGLV_INFO:
        return "INFO";
    case LOGLV_DEBUG:
        return "DEBUG";
    default:
        break;
    }
    return "UNKNOWN";
}
static void _worker_init(logworker_ctx *ctx) {
    ctx->file = NULL;
    ZERO(ctx->path, sizeof(ctx->path));
    ZERO(ctx->lognam, sizeof(ctx->lognam));
    //´´½¨ÎÄ¼þ¼Ð
    strcpy(ctx->path, procpath());
    size_t len = strlen(ctx->path);
    SNPRINTF(ctx->path + len, sizeof(ctx->path) - len - 1, "%s%s", PATH_SEPARATORSTR, LOG_FOLDER);
    if (ERR_OK != ACCESS(ctx->path, 0)) {
        if (ERR_OK != MKDIR(ctx->path)) {
            PRINT("mkdir(%s) failed.", ctx->path);
            ctx->path[0] = '\0';
        }
    }
#if defined(OS_WIN)
    //»ñÈ¡±ê×¼Êä³ö
    ctx->hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (INVALID_HANDLE_VALUE == ctx->hout
        || NULL == ctx->hout) {
        ctx->hout = NULL;
        PRINT("%s", "GetStdHandle(STD_OUTPUT_HANDLE) error.");
        return;
    }
    if (!GetConsoleScreenBufferInfo(ctx->hout, &ctx->stcsbi)) {
        PRINT("%s", "GetConsoleScreenBufferInfo error.");
        return;
    }
#endif  
}
static void _worker_free(logworker_ctx *ctx) {
    if (NULL != ctx->file) {
        fclose(ctx->file);
        ctx->file = NULL;
    }
}
static int32_t _worker_getfile(logworker_ctx *ctx) {
    if (0 == strlen(ctx->path)) {
        return ERR_FAILED;
    }
    //ÅÐ¶ÏÈÕÆÚÊÇ·ñ¸ü¸Ä£¬¸ü¸ÄÔò¸ü»»logÎÄ¼þÃû
    char date[TIME_LENS] = {0};
    nowtime("%Y-%m-%d", date);
    if (0 != strcmp(date, ctx->lognam)) {
        if (NULL != ctx->file) {
            fclose(ctx->file);
            ctx->file = NULL;
        }
        size_t lens = strlen(date);
        memcpy(ctx->lognam, date, lens);
        ctx->lognam[lens] = '\0';
    }
    //´ò¿ªÎÄ¼þ
    if (NULL == ctx->file) {
        char fpath[PATH_LENS] = { 0 };
        SNPRINTF(fpath, sizeof(fpath) - 1, "%s%s%s%s", ctx->path, PATH_SEPARATORSTR, ctx->lognam, ".log");
        ctx->file = fopen(fpath, "a");
        if (NULL == ctx->file) {
            PRINT("fopen(%s, a) error.", fpath);
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
static void _worker_writelog(logworker_ctx *ctx, loginfo_ctx *info) {
    static const char *pn = "\n";
    (void)fwrite(info->time, 1, strlen(info->time), ctx->file);
    (void)fwrite(info->data, 1, strlen((const char *)info->data), ctx->file);
    (void)fwrite(pn, 1, strlen(pn), ctx->file);
}
static void _worker_printlog(logworker_ctx *ctx, loginfo_ctx *info) {
    if (0 == ctx->ploger->prt) {
        return;
    }
#if defined(OS_WIN)
    if (NULL == ctx->hout) {
        return;
    }
    switch (info->lv) {
    case LOGLV_FATAL:
        SetConsoleTextAttribute(ctx->hout, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | BACKGROUND_RED);
        break;
    case LOGLV_ERROR:
        SetConsoleTextAttribute(ctx->hout, FOREGROUND_RED);
        break;
    case LOGLV_WARN:
        SetConsoleTextAttribute(ctx->hout, FOREGROUND_RED | FOREGROUND_GREEN);
        break;
    case LOGLV_INFO:
        SetConsoleTextAttribute(ctx->hout, FOREGROUND_GREEN);
        break;
    case LOGLV_DEBUG:
        SetConsoleTextAttribute(ctx->hout, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        break;
    default:
        break;
    }
    printf("%s%s\n", info->time, (const char *)info->data);
    SetConsoleTextAttribute(ctx->hout, ctx->stcsbi.wAttributes);
#else
    switch (info->lv) {
    case LOGLV_FATAL:
        printf("%s%s%s%s\n", CLR_RED_WHT, info->time, (const char *)info->data, CLR_CLR);
        break;
    case LOGLV_ERROR:
        printf("%s%s%s%s\n", CLR_RED, info->time, (const char *)info->data, CLR_CLR);
        break;
    case LOGLV_WARN:
        printf("%s%s%s%s\n", CLR_YELLOW, info->time, (const char *)info->data, CLR_CLR);
        break;
    case LOGLV_INFO:
        printf("%s%s%s%s\n", CLR_GREEN, info->time, (const char *)info->data, CLR_CLR);
        break;
    case LOGLV_DEBUG:
        printf("%s%s%s%s\n", CLR_WHITE, info->time, (const char *)info->data, CLR_CLR);
        break;
    }
#endif
}
static void _loger(void *arg) {
    logworker_ctx stworker;
    stworker.ploger = (loger_ctx *)arg;
    _worker_init(&stworker);
    loginfo_ctx *info;
    while (0 == stworker.ploger->stop) {
        if (ERR_OK != chan_recv(&stworker.ploger->chan, (void **)&info)) {
            continue;
        }
        _worker_printlog(&stworker, info);
        if (ERR_OK != _worker_getfile(&stworker)) {
            FREE(info->data);
            FREE(info);
            continue;
        }
        _worker_writelog(&stworker, info);
        FREE(info->data);
        FREE(info);
    }
    _worker_free(&stworker);
}

void loger_init(loger_ctx *ctx) {
    ctx->stop = 0;
    ctx->lv = LOGLV_DEBUG;
    ctx->prt = 1;
    chan_init(&ctx->chan, ONEK * 4);
    ctx->thloger = thread_creat(_loger, ctx);
}
void loger_free(loger_ctx *ctx) {
    chan_close(&ctx->chan);
    while (chan_size(&ctx->chan) > 0);
    ctx->stop = 1;
    thread_join(ctx->thloger);
    chan_free(&ctx->chan);
}
void loger_setlv(loger_ctx *ctx, const LOG_LEVEL lv) {
    ctx->lv = lv;
}
void loger_setprint(loger_ctx *ctx, const int32_t prt) {
    ctx->prt = prt;
}
static void _log_time(char time[TIME_LENS]) {
    struct timeval tv;
    timeofday(&tv);
    time_t t = tv.tv_sec;
    strftime(time, TIME_LENS - 1, "[%H:%M:%S", localtime(&t));
    size_t len = strlen(time);
    SNPRINTF(time + len, TIME_LENS - len - 1, " %03d]", (int32_t)(tv.tv_usec / 1000));
}
void loger_log(loger_ctx *ctx, const LOG_LEVEL lv, const char *fmt, ...) {
    if (lv > ctx->lv) {
        return;
    }
    loginfo_ctx *info;
    MALLOC(info, sizeof(loginfo_ctx));
    info->lv = lv;
    ZERO(info->time, sizeof(info->time));
    _log_time(info->time);
    va_list va;
    va_start(va, fmt);
    info->data = formatargs(fmt, va);
    va_end(va);
    if (ERR_OK != chan_send(&ctx->chan, info)) {
        FREE(info->data);
        FREE(info);
    }
}
