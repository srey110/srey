#ifndef LOGER_H_
#define LOGER_H_

#include "chan.h"
#include "thread.h"
#include "mutex.h"

typedef enum LOG_LEVEL {
    LOGLV_FATAL = 0,
    LOGLV_ERROR,
    LOGLV_WARN,
    LOGLV_INFO,
    LOGLV_DEBUG,
}LOG_LEVEL;
typedef struct loger_ctx {
    int32_t lv;
    int32_t prt;
    volatile int32_t stop;
    chan_ctx chan;
    pthread_t thloger;    
}loger_ctx;

void loger_init(loger_ctx *ctx);
void loger_free(loger_ctx *ctx);
void loger_setlv(loger_ctx *ctx, const LOG_LEVEL lv);
void loger_setprint(loger_ctx *ctx, const int32_t prt);
void loger_log(loger_ctx *ctx, const LOG_LEVEL lv, const char *fmt, ...);
const char *_getlvstr(const LOG_LEVEL lv);

extern loger_ctx g_logerctx;
#define LOGINIT() loger_init(&g_logerctx)
#define LOGFREE() loger_free(&g_logerctx)
#define SETLOGLV(lv) loger_setlv(&g_logerctx, lv)
#define SETLOGPRT(prt) loger_setprint(&g_logerctx, prt)
#define LOG(lv,fmt, ...)\
    (loger_log(&g_logerctx, lv, CONCAT2("[%s][%s %s %d]", fmt), \
     _getlvstr(lv), __FILENAME__(__FILE__), __FUNCTION__, __LINE__, ##__VA_ARGS__))
#define LOG_FATAL(fmt, ...) LOG(LOGLV_FATAL, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG(LOGLV_ERROR, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  LOG(LOGLV_WARN, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  LOG(LOGLV_INFO, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) LOG(LOGLV_DEBUG, fmt, ##__VA_ARGS__)

#endif//LOGER_H_
