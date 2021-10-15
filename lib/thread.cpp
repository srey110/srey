#include "thread.h"
#include "utils.h"

SREY_NS_BEGIN

#ifdef OS_WIN
uint32_t __stdcall _taskcb(void *parg)
#else
void *_taskcb(void *parg)
#endif
{
    cthread *pthread = (cthread *)parg;
    pthread->_setid((ATOMIC_T)threadid());
    volatile ATOMIC_T *pstate = pthread->_getstate();
    ATOMIC_SET(pstate, THREAD_RUNING);

    ctask *ptask = pthread->_gettask();
    ptask->beforrun();
    ptask->run();
    ptask->afterrun();

    ATOMIC_SET(pstate, THREAD_STOP);
#ifdef OS_WIN
    return ERR_OK;
#else
    return NULL;
#endif
}
#ifdef OS_WIN
uint32_t __stdcall _funccb(void *parg)
#else
void *_funccb(void *parg)
#endif
{
    cthread *pthread = (cthread *)parg;
    pthread->_setid((ATOMIC_T)threadid());
    volatile ATOMIC_T *pstate = pthread->_getstate();
    ATOMIC_SET(pstate, THREAD_RUNING);

    pthread->_getfunc()(pthread->_getparam());

    ATOMIC_SET(pstate, THREAD_STOP);
#ifdef OS_WIN
    return ERR_OK;
#else
    return NULL;
#endif
}

cthread::cthread()
{
    m_threadid = INIT_NUMBER;
    m_state = THREAD_STOP;
}
cthread::~cthread()
{
    join();
}
void cthread::creat(ctask *ptask)
{
    if (THREAD_STOP != ATOMIC_CAS(&m_state, THREAD_STOP, THREAD_WAITRUN))
    {
        PRINTF("%s", "thread not stop.");
        return;
    }

    m_task = ptask;

#ifdef OS_WIN
    m_thread = (HANDLE)_beginthreadex(NULL, 0, _taskcb, this, 0, NULL);
    ASSERTAB(NULL != m_thread, ERRORSTR(ERRNO));
#else
    ASSERTAB((ERR_OK == pthread_create(&m_thread, NULL, _taskcb, (void*)this)),
        ERRORSTR(ERRNO));
#endif
}
void cthread::creat(thread_cb func, void *pparam)
{
    if (THREAD_STOP != ATOMIC_CAS(&m_state, THREAD_STOP, THREAD_WAITRUN))
    {
        PRINTF("%s", "thread not stop.");
        return;
    }

    m_func = func;
    m_param = pparam;

#ifdef OS_WIN
    m_thread = (HANDLE)_beginthreadex(NULL, 0, _funccb, this, 0, NULL);
    ASSERTAB(NULL != m_thread, ERRORSTR(ERRNO));
#else
    ASSERTAB((ERR_OK == pthread_create(&m_thread, NULL, _funccb, (void*)this)),
        ERRORSTR(ERRNO));
#endif
}
void cthread::waitstart()
{
    while (THREAD_WAITRUN == ATOMIC_GET(&m_state));
}
void cthread::join()
{
    if (THREAD_STOP == ATOMIC_GET(&m_state))
    {
        return;
    }
#ifdef OS_WIN
    ASSERTAB(WAIT_OBJECT_0 == WaitForSingleObject(m_thread, INFINITE), ERRORSTR(ERRNO));
#else
    ASSERTAB(ERR_OK == pthread_join(m_thread, NULL), ERRORSTR(ERRNO));
#endif
}

SREY_NS_END
