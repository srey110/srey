#ifndef THREAD_H_
#define THREAD_H_

#include "macro.h"

SREY_NS_BEGIN

class ctask
{
public:
    ctask() : m_stop(INIT_NUMBER)
    {};
    virtual ~ctask() {};
    virtual void beforrun() {};
    virtual void run() {};
    virtual void afterrun() {};

    bool isstop()
    {
        return INIT_NUMBER != ATOMIC_GET(&m_stop);
    };
    void stop()
    {
        ATOMIC_SET(&m_stop, 1);
    };

private:
    volatile ATOMIC_T m_stop;
};

#define THREAD_WAITRUN  0
#define THREAD_RUNING   1
#define THREAD_STOP     2
typedef void(*thread_cb)(void*);//线程回调函数

class cthread
{
public:
    cthread();
    ~cthread();
    /*
    * \brief             创建一线程,别多次调用
    * \param ptask       ctask
    */
    void creat(ctask *ptask);
    /*
    * \brief             创建一线程,别多次调用
    * \param func        回调函数
    * \param pparam      回调函数参数
    */
    void creat(thread_cb func, void *pparam);
    /*
    * \brief          等待线程启动
    */
    void waitstart();
    /*
    * \brief          等待线程结束
    */
    void join();
    /*
    * \brief          获取启动的线程id
    */    
    uint32_t getid()
    {
        return (uint32_t)ATOMIC_GET(&m_threadid);
    };
    
    void _setid(const ATOMIC_T &uiid)
    {
        ATOMIC_SET(&m_threadid, uiid);
    };
    volatile ATOMIC_T *_getstate()
    {
        return &m_state;
    };
    ctask *_gettask()
    {
        return m_task;
    };
    thread_cb _getfunc()
    {
        return m_func;
    };
    void *_getparam()
    {
        return m_param;
    };
private:
    volatile ATOMIC_T m_threadid;
    volatile ATOMIC_T m_state;
    
#ifdef OS_WIN
    HANDLE m_thread;
#else
    pthread_t m_thread;//线程句柄
#endif
    ctask *m_task;
    void *m_param;
    thread_cb m_func;
};

SREY_NS_END

#endif//THREAD_H_
