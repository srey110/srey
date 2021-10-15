#ifndef RWLOCK_H_
#define RWLOCK_H_

#include "macro.h"

SREY_NS_BEGIN

class crwlock
{
public:
    crwlock()
    {
#ifdef OS_WIN
        InitializeSRWLock(&m_rwlock);
        m_wlock = false;
#else
        ASSERTAB((ERR_OK == pthread_rwlock_init(&m_rwlock, NULL)),
            ERRORSTR(ERRNO));
#endif
    };
    ~crwlock()
    {
#ifndef OS_WIN
        (void)pthread_rwlock_destroy(&m_rwlock);
#endif
    };

    /*
    * \brief          读锁定
    */
    void rdlock()
    {
#ifdef OS_WIN
        AcquireSRWLockShared(&m_rwlock);
#else
        ASSERTAB(ERR_OK == pthread_rwlock_rdlock(&m_rwlock), ERRORSTR(ERRNO));
#endif
    };
    /*
    * \brief          非阻塞读锁定
    * \return         true 成功
    */
    bool tryrdlock()
    {
#ifdef OS_WIN
        return INIT_NUMBER != TryAcquireSRWLockShared(&m_rwlock);
#else
        return ERR_OK == pthread_rwlock_tryrdlock(&m_rwlock);
#endif
    };
    /*
    * \brief          写锁定
    * \return         true 成功
    */
    void wrlock()
    {
#ifdef OS_WIN
        AcquireSRWLockExclusive(&m_rwlock);
        m_wlock = true;
#else
        ASSERTAB(ERR_OK == pthread_rwlock_wrlock(&m_rwlock), ERRORSTR(ERRNO));
#endif
    };
    /*
    * \brief          非阻塞写锁定
    * \return         true 成功
    */
    bool trywrlock()
    {
#ifdef OS_WIN
        bool bret = (INIT_NUMBER != TryAcquireSRWLockExclusive(&m_rwlock));
        if (bret)
        {
            m_wlock = true;
        }

        return bret;
#else
        return ERR_OK == pthread_rwlock_trywrlock(&m_rwlock);
#endif
    };
    /*
    * \brief          解锁
    */
    void unlock()
    {
#ifdef OS_WIN
        if (m_wlock)
        {
            m_wlock = false;
            ReleaseSRWLockExclusive(&m_rwlock);
        }
        else
        {
            ReleaseSRWLockShared(&m_rwlock);
        }
#else
        ASSERTAB(ERR_OK == pthread_rwlock_unlock(&m_rwlock), ERRORSTR(ERRNO));
#endif
    };

private:
#ifdef OS_WIN
    SRWLOCK m_rwlock;
    bool m_wlock;
#else
    pthread_rwlock_t m_rwlock;
#endif
};

SREY_NS_END

#endif//RWLOCK_H_
