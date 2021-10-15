#ifndef SPINLOCK_H_
#define SPINLOCK_H_

#include "macro.h"

SREY_NS_BEGIN

class cspinlock
{
public:
    cspinlock(const uint32_t &uispcount = ONEK)
    {
#ifdef OS_WIN
        ASSERTAB(InitializeCriticalSectionAndSpinCount(&m_spin, uispcount),
            "InitializeCriticalSectionAndSpinCount error.");
#else
        ASSERTAB(ERR_OK == pthread_spin_init(&m_spin, PTHREAD_PROCESS_PRIVATE), ERRORSTR(ERRNO));
#endif
    };
    ~cspinlock()
    {
#ifdef OS_WIN
        DeleteCriticalSection(&m_spin);
#else
        (void)pthread_spin_destroy(&m_spin);
#endif
    };

    /*
    * \brief          锁定
    */
    void lock()
    {
#ifdef OS_WIN
        EnterCriticalSection(&m_spin);
#else
        ASSERTAB(ERR_OK == pthread_spin_lock(&m_spin), ERRORSTR(ERRNO));
#endif
    };
    /*
    * \brief          非阻塞锁定
    * \return         true 成功
    */
    bool trylock()
    {
#ifdef OS_WIN
        return TRUE == TryEnterCriticalSection(&m_spin);
#else
        return ERR_OK == pthread_spin_trylock(&m_spin);
#endif
    };
    /*
    * \brief          解锁
    */
    void unlock()
    {
#ifdef OS_WIN
        LeaveCriticalSection(&m_spin);
#else
        ASSERTAB(ERR_OK == pthread_spin_unlock(&m_spin), ERRORSTR(ERRNO));
#endif
    };

private:
#ifdef OS_WIN
    CRITICAL_SECTION m_spin;
#else
    pthread_spinlock_t m_spin;
#endif
};

SREY_NS_END

#endif//SPINLOCK_H_
