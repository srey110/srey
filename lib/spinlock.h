#ifndef SPINLOCK_H_
#define SPINLOCK_H_

#include "macro.h"

#if defined(OS_WIN)
typedef CRITICAL_SECTION spin_ctx;
#elif defined(OS_DARWIN)
typedef os_unfair_lock spin_ctx;
#else
typedef pthread_spinlock_t spin_ctx;
#endif
/*
* \brief          初始化  ONEK
*/
void spin_init(spin_ctx *pctx, const uint32_t uispcnt);
/*
* \brief          释放
*/
void spin_free(spin_ctx *pctx);
/*
* \brief          锁定
*/
void spin_lock(spin_ctx *pctx);
/*
* \brief          非阻塞锁定
* \return         ERR_OK 成功
*/
int32_t spin_trylock(spin_ctx *pctx);
/*
* \brief          解锁
*/
void spin_unlock(spin_ctx *pctx);

#endif//SPINLOCK_H_
