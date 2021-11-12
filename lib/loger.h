#ifndef LOGER_H_
#define LOGER_H_

#include "chan.h"
#include "thread.h"
#include "mutex.h"

typedef enum LOG_LEVEL
{
    LOGLV_FATAL = 0,
    LOGLV_ERROR,
    LOGLV_WARN,
    LOGLV_INFO,
    LOGLV_DEBUG,
}LOG_LEVEL;
struct loger_ctx
{
    int32_t stop;
    int32_t lv;
    int32_t print;
    struct chan_ctx chan;
    struct thread_ctx thloger;    
};
/*
* \brief  初始化
*/
void loger_init(struct loger_ctx *pctx);
/*
* \brief  释放
*/
void loger_free(struct loger_ctx *pctx);
/*
* \brief  设置日志级别，大于该级别的，不记录,支持动态调整
*/
void loger_setlv(struct loger_ctx *pctx, const LOG_LEVEL emlv);
/*
* \brief         设置是否打印到屏幕,支持动态调整
* \param iprint  0 不打印  1 打印
*/
void loger_setprint(struct loger_ctx *pctx, const int32_t iprint);
/*
* \brief          日志
* \param emlv     emlv 日志级别
* \param pformat  格式化字符
*/
void loger_log(struct loger_ctx *pctx, const LOG_LEVEL emlv, const char *pformat, ...);
const char *_getlvstr(const LOG_LEVEL emlv);

extern struct loger_ctx g_logerctx;

#define LOGINIT() loger_init(&g_logerctx)
#define LOGFREE() loger_free(&g_logerctx)
#define SETLOGLV(lv) loger_setlv(&g_logerctx, lv)
#define SETLOGPRT(bprt) loger_setprint(&g_logerctx, bprt)
#define LOG(lv,format, ...)\
    (loger_log(&g_logerctx, lv, CONCAT2("[%s][%s %s %d]", format), \
     _getlvstr(lv), __FILENAME__, __FUNCTION__, __LINE__, ##__VA_ARGS__))
#undef LOG_FATAL
#define LOG_FATAL(format, ...) LOG(LOGLV_FATAL, format, ##__VA_ARGS__)
#undef LOG_ERROR
#define LOG_ERROR(format, ...) LOG(LOGLV_ERROR, format, ##__VA_ARGS__)
#undef LOG_WARN
#define LOG_WARN(format, ...)  LOG(LOGLV_WARN, format, ##__VA_ARGS__)
#undef LOG_INFO
#define LOG_INFO(format, ...)  LOG(LOGLV_INFO, format, ##__VA_ARGS__)
#undef LOG_DEBUG
#define LOG_DEBUG(format, ...) LOG(LOGLV_DEBUG, format, ##__VA_ARGS__)

#endif//LOGER_H_
