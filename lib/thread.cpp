#include "thread.h"
#include "utils.h"
#include "errcode.h"

SREY_NS_BEGIN

#ifdef OS_WIN
uint32_t __stdcall _taskcb(void *parg)
#else
void *_taskcb(void *parg)
#endif
{
    cthread *pthread = (cthread *)parg;
    pthread->_setid(threadid());
    uint32_t *pstart = pthread->_getstart();
    ATOMIC_SET(pstart, THREAD_RUNING);

    ctask *ptask = pthread->_gettask();
    ptask->beforrun();
    ptask->run();
    ptask->afterrun();

    ATOMIC_SET(pstart, THREAD_STOP);
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
    pthread->_setid(threadid());
    uint32_t *pstart = pthread->_getstart();
    ATOMIC_SET(pstart, THREAD_RUNING);

    pthread->_getfunc()(pthread->_getparam());

    ATOMIC_SET(pstart, THREAD_STOP);
#ifdef OS_WIN
    return ERR_OK;
#else
    return NULL;
#endif
}

cthread::cthread()
{
    threadid = INIT_NUMBER;
    start = THREAD_STOP;
}
void cthread::creat(ctask *ptask)
{
    if (THREAD_STOP != state())
    {
        PRINTF("%s", "thread not stop.");
        return;
    }
    ATOMIC_SET(&start, THREAD_WAITRUN);

    task = ptask;

#ifdef OS_WIN
    pthread = (HANDLE)_beginthreadex(NULL, 0, _taskcb, this, 0, NULL);
    ASSERTAB(NULL != pthread, ERRORSTR(ERRNO));
#else
    ASSERTAB((ERR_OK == pthread_create(&pthread, NULL, _taskcb, (void*)this)),
        ERRORSTR(ERRNO));
#endif
}
void cthread::creat(thread_cb func, void *pparam)
{
    if (THREAD_STOP != state())
    {
        PRINTF("%s", "thread not stop.");
        return;
    }
    ATOMIC_SET(&start, THREAD_WAITRUN);

    funccb = func;
    param = pparam;

#ifdef OS_WIN
    pthread = (HANDLE)_beginthreadex(NULL, 0, _funccb, this, 0, NULL);
    ASSERTAB(NULL != pthread, ERRORSTR(ERRNO));
#else
    ASSERTAB((ERR_OK == pthread_create(&pthread, NULL, _funccb, (void*)this)),
        ERRORSTR(ERRNO));
#endif
}
uint32_t cthread::state()
{
    return ATOMIC_GET(&start);
}
void cthread::waitstart()
{
    while (THREAD_WAITRUN == state());
}
void cthread::join()
{
    if (THREAD_STOP == state())
    {
        return;
    }
#ifdef OS_WIN
    ASSERTAB(WAIT_OBJECT_0 == WaitForSingleObject(pthread, INFINITE), ERRORSTR(ERRNO));
#else
    ASSERTAB(ERR_OK == pthread_join(pthread, NULL), ERRORSTR(ERRNO));
#endif
}

SREY_NS_END
