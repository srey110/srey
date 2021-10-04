#include "rwlock.h"
#include "errcode.h"

SREY_NS_BEGIN

crwlock::crwlock()
{
#ifdef OS_WIN
    InitializeSRWLock(&lock);
    exclusive = false;
#else
    ASSERTAB((ERR_OK == pthread_rwlock_init(&lock, NULL)),
        "pthread_rwlock_init error.");
#endif
}
crwlock::~crwlock()
{
#ifndef OS_WIN
    (void)pthread_rwlock_destroy(&lock);
#endif
}
void crwlock::rdlock()
{
#ifdef OS_WIN
    AcquireSRWLockShared(&lock);
#else
    (void)pthread_rwlock_rdlock(&lock);
#endif
}
bool crwlock::tryrdlock()
{
#ifdef OS_WIN
    return INIT_NUMBER != TryAcquireSRWLockShared(&lock);
#else
    return ERR_OK == pthread_rwlock_tryrdlock(&lock);
#endif
}
void crwlock::wrlock()
{
#ifdef OS_WIN
    AcquireSRWLockExclusive(&lock);
    exclusive = true;
#else
    (void)pthread_rwlock_wrlock(&lock);
#endif
}
bool crwlock::trywrlock()
{
#ifdef OS_WIN
    bool bret = (INIT_NUMBER != TryAcquireSRWLockExclusive(&lock));
    if (bret)
    {
        exclusive = true;
    }

    return bret;
#else
    return ERR_OK == pthread_rwlock_trywrlock(&lock);
#endif
}
void crwlock::unlock()
{
#ifdef OS_WIN
    if (exclusive)
    {
        exclusive = false;
        ReleaseSRWLockExclusive(&lock);
    }
    else
    {
        ReleaseSRWLockShared(&lock);
    }
#else
    (void)pthread_rwlock_unlock(&lock);
#endif
}

SREY_NS_END
