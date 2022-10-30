#ifndef RWLOCK_H_
#define RWLOCK_H_

#include "macro.h"

struct rwlock_ctx
{
#if defined(OS_WIN)
    uint32_t wlock;
    SRWLOCK rwlock;
#else
    pthread_rwlock_t rwlock;
#endif
};
/*
* \brief          初始化
*/
void rwlock_init(struct rwlock_ctx *pctx);
/*
* \brief          释放
*/
void rwlock_free(struct rwlock_ctx *pctx);

/*
* \brief          读锁定
*/
void rwlock_rdlock(struct rwlock_ctx *pctx);
/*
* \brief          非阻塞读锁定
* \return         ERR_OK 成功
*/
int32_t rwlock_tryrdlock(struct rwlock_ctx *pctx);
/*
* \brief          写锁定
* \return         true 成功
*/
void rwlock_wrlock(struct rwlock_ctx *pctx);
/*
* \brief          非阻塞写锁定
* \return         ERR_OK 成功
*/
int32_t rwlock_trywrlock(struct rwlock_ctx *pctx);
/*
* \brief          解锁
*/
void rwlock_unlock(struct rwlock_ctx *pctx);

#endif//RWLOCK_H_
