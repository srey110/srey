#include "spinlock.h"

SREY_NS_BEGIN

cspinlock::cspinlock()
{
#ifdef OS_WIN
    ASSERTAB(InitializeCriticalSectionAndSpinCount(&spin, -1),
        "InitializeCriticalSectionAndSpinCount error.");
#else
    ASSERTAB(ERR_OK == pthread_spin_init(&spin, PTHREAD_PROCESS_PRIVATE), "pthread_spin_init error.");
#endif
}
cspinlock::~cspinlock()
{
#ifdef OS_WIN
    DeleteCriticalSection(&spin);
#else
    (void)pthread_spin_destroy(&spin);
#endif
}
void cspinlock::lock()
{
#ifdef OS_WIN
    EnterCriticalSection(&spin);
#else
    (void)pthread_spin_lock(&spin);
#endif
}
bool cspinlock::trylock()
{
#ifdef OS_WIN
    return TRUE == TryEnterCriticalSection(&spin);
#else
    return ERR_OK == pthread_spin_trylock(&spin);
#endif
}
void cspinlock::unlock()
{
#ifdef OS_WIN
    LeaveCriticalSection(&spin);
#else
    (void)pthread_spin_unlock(&spin);
#endif
}

SREY_NS_END
