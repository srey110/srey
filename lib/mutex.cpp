#include "mutex.h"
#include "errcode.h"

SREY_NS_BEGIN

cmutex::cmutex()
{
#ifdef OS_WIN
    InitializeCriticalSection(&mutex);
#else
    ASSERTAB(ERR_OK == pthread_mutex_init(&mutex, (const pthread_mutexattr_t*)NULL), 
        "pthread_mutex_init error.");
#endif
}
cmutex::~cmutex()
{
#ifdef OS_WIN
    DeleteCriticalSection(&mutex);
#else
    (void)pthread_mutex_destroy(&mutex);
#endif
}
void cmutex::lock()
{
#ifdef OS_WIN
    EnterCriticalSection(&mutex);
#else
    (void)pthread_mutex_lock(&mutex);
#endif
}
bool cmutex::trylock()
{
#ifdef OS_WIN
    return TRUE == TryEnterCriticalSection(&mutex);
#else
    return ERR_OK == pthread_mutex_trylock(&mutex);
#endif
}
void cmutex::unlock()
{
#ifdef OS_WIN
    LeaveCriticalSection(&mutex);
#else
    (void)pthread_mutex_unlock(&mutex);
#endif
}

SREY_NS_END
