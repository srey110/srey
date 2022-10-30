#ifndef COND_H_
#define COND_H_

#include "mutex.h"

#if defined(OS_WIN)
typedef CONDITION_VARIABLE cond_ctx;
#else
typedef pthread_cond_t cond_ctx;
#endif
/*
* \brief          初始化
*/
void cond_init(cond_ctx *pctx);
/*
* \brief          释放
*/
void cond_free(cond_ctx *pctx);
/*
* \brief          等待信号量
*/
void cond_wait(cond_ctx *pctx, mutex_ctx *pmutex);
/*
* \brief          等待信号量
* \param uims     等待时间  毫秒
*/
void cond_timedwait(cond_ctx *pctx, mutex_ctx *pmutex, const uint32_t uims);
/*
* \brief          激活信号
*/
void cond_signal(cond_ctx *pctx);
/*
* \brief          激活所有信号
*/
void cond_broadcast(cond_ctx *pctx);

#endif//COND_H_
