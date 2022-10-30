#ifndef MUTEX_H_
#define MUTEX_H_

#include "macro.h"

#if defined(OS_WIN)
typedef CRITICAL_SECTION mutex_ctx;
#else
typedef pthread_mutex_t mutex_ctx;
#endif
/*
* \brief          初始化
*/
void mutex_init(mutex_ctx *pctx);
/*
* \brief          释放
*/
void mutex_free(mutex_ctx *pctx);
/*
* \brief          锁定
*/
void mutex_lock(mutex_ctx *pctx);
/*
* \brief          try锁定
* \return         ERR_OK 成功
*/
int32_t mutex_trylock(mutex_ctx *pctx);
/*
* \brief          解锁
*/
void mutex_unlock(mutex_ctx *pctx);

#endif//MUTEX_H_
