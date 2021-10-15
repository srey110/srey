#ifndef MUTEX_H_
#define MUTEX_H_

#include "macro.h"

SREY_NS_BEGIN

class cmutex
{
public:
    cmutex() 
    {
#ifdef OS_WIN
        InitializeCriticalSection(&m_mutex);
#else
        ASSERTAB(ERR_OK == pthread_mutex_init(&m_mutex, (const pthread_mutexattr_t*)NULL),
            ERRORSTR(ERRNO));
#endif
    };
    ~cmutex()
    {
#ifdef OS_WIN
        DeleteCriticalSection(&m_mutex);
#else
        (void)pthread_mutex_destroy(&m_mutex);
#endif
    };

    /*
    * \brief          锁定
    */
    void lock()
    {
#ifdef OS_WIN
        EnterCriticalSection(&m_mutex);
#else
        ASSERTAB(ERR_OK == pthread_mutex_lock(&m_mutex), ERRORSTR(ERRNO));
#endif
    };
    /*
    * \brief          非阻塞锁定
    * \return         true 成功
    */
    bool trylock()
    {
#ifdef OS_WIN
        return TRUE == TryEnterCriticalSection(&m_mutex);
#else
        return ERR_OK == pthread_mutex_trylock(&m_mutex);
#endif
    };
    /*
    * \brief          解锁
    */
    void unlock()
    {
#ifdef OS_WIN
        LeaveCriticalSection(&m_mutex);
#else
        ASSERTAB(ERR_OK == pthread_mutex_unlock(&m_mutex), ERRORSTR(ERRNO));
#endif
    };

#ifdef OS_WIN
    CRITICAL_SECTION *getmutex()
#else
    pthread_mutex_t *getmutex()
#endif    
    {
        return &m_mutex;
    };

private:
#ifdef OS_WIN 
    CRITICAL_SECTION m_mutex;
#else
    pthread_mutex_t m_mutex;
#endif    
};

SREY_NS_END

#endif//MUTEX_H_
