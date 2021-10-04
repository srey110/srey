#include "cond.h"
#include "errcode.h"

SREY_NS_BEGIN

ccond::ccond()
{
#ifdef OS_WIN
    InitializeConditionVariable(&cond);
#else
    ASSERTAB(ERR_OK == pthread_cond_init(&cond, (const pthread_condattr_t*)NULL), 
        "pthread_cond_init error.");
#endif
}
ccond::~ccond()
{
#ifndef OS_WIN
    (void)pthread_cond_destroy(&cond);
#endif
}
void ccond::wait(cmutex *pmu)
{
#ifdef OS_WIN
    ASSERTAB(SleepConditionVariableCS(&cond, pmu->getmutex(), INFINITE),
        "SleepConditionVariableCS error.");
#else
    ASSERTAB(ERR_OK == pthread_cond_wait(&cond, pmu->getmutex()),
        "pthread_cond_wait error.");
#endif
}
void ccond::timedwait(cmutex *pmu, const uint32_t &uims)
{
#ifdef OS_WIN
    BOOL brtn = SleepConditionVariableCS(&cond, pmu->getmutex(), (DWORD)uims);
    if (!brtn)
    {
        ASSERTAB(ERROR_TIMEOUT == LASTERROR(), "SleepConditionVariableCS error.");
    }
#else
    long seconds = uims / 1000;
    long nanoseconds = (uims - seconds * 1000) * 1000000;

    struct timeval now;
    gettimeofday(&now, NULL);
    
    struct timespec timewait;
    timewait.tv_sec = now.tv_sec + seconds;
    timewait.tv_nsec = now.tv_usec * 1000 + nanoseconds;
    if (timewait.tv_nsec >= 1000000000)
    {
        timewait.tv_nsec -= 1000000000;
        timewait.tv_sec++;
    }

    int32_t irtn = pthread_cond_timedwait(&cond, pmu->getmutex(), &timewait);
    ASSERTAB((ERR_OK == irtn || ETIMEDOUT == irtn), "pthread_cond_timedwait error.");
#endif
}
void ccond::signal()
{
#ifdef OS_WIN
    WakeConditionVariable(&cond);
#else
    (void)pthread_cond_signal(&cond);
#endif
}
void ccond::broadcast()
{
#ifdef OS_WIN
    WakeAllConditionVariable(&cond);
#else
    (void)pthread_cond_broadcast(&cond);
#endif
}

SREY_NS_END
