#ifndef MUTEX_H_
#define MUTEX_H_

#include "base/macro.h"

#if defined(OS_WIN)
typedef CRITICAL_SECTION mutex_ctx;
#else
typedef pthread_mutex_t mutex_ctx;
#endif
/// <summary>
/// ��������ʼ��
/// </summary>
/// <param name="ctx">mutex_ctx</param>
static inline void mutex_init(mutex_ctx *ctx) {
#if defined(OS_WIN)
    InitializeCriticalSection(ctx);
#else
    ASSERTAB(ERR_OK == pthread_mutex_init(ctx, (const pthread_mutexattr_t*)NULL),
        ERRORSTR(ERRNO));
#endif
};
/// <summary>
/// �������ͷ�
/// </summary>
/// <param name="ctx">mutex_ctx</param>
static inline void mutex_free(mutex_ctx *ctx) {
#if defined(OS_WIN)
    DeleteCriticalSection(ctx);
#else
    (void)pthread_mutex_destroy(ctx);
#endif
};
/// <summary>
/// ����
/// </summary>
/// <param name="ctx">mutex_ctx</param>
static inline void mutex_lock(mutex_ctx *ctx) {
#if defined(OS_WIN)
    EnterCriticalSection(ctx);
#else
    ASSERTAB(ERR_OK == pthread_mutex_lock(ctx), ERRORSTR(ERRNO));
#endif
};
/// <summary>
/// ��������
/// </summary>
/// <param name="ctx">mutex_ctx</param>
/// <returns>ERR_OK �ɹ�</returns>
static inline int32_t mutex_trylock(mutex_ctx *ctx) {
#if defined(OS_WIN)
    return TRUE == TryEnterCriticalSection(ctx) ? ERR_OK : ERR_FAILED;
#else
    return pthread_mutex_trylock(ctx);
#endif
};
/// <summary>
/// ����
/// </summary>
/// <param name="ctx">mutex_ctx</param>
static inline void mutex_unlock(mutex_ctx *ctx) {
#if defined(OS_WIN)
    LeaveCriticalSection(ctx);
#else
    ASSERTAB(ERR_OK == pthread_mutex_unlock(ctx), ERRORSTR(ERRNO));
#endif
};

#endif//MUTEX_H_
