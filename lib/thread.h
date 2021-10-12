#ifndef THREAD_H_
#define THREAD_H_

#include "macro.h"

SREY_NS_BEGIN

class ctask
{
public:
    ctask() : uiloop(INIT_NUMBER)
    {};
    virtual ~ctask() {};
    virtual void beforrun() {};
    virtual void run() {};
    virtual void afterrun() {};

    bool loop()
    {
        return INIT_NUMBER == ATOMIC_GET(&uiloop);
    };
    void stoploop()
    {
        ATOMIC_SET(&uiloop, 1);
    };

private:
    volatile ATOMIC_T uiloop;
};

#define THREAD_WAITRUN  0
#define THREAD_RUNING   1
#define THREAD_STOP     2

typedef void(*thread_cb)(void*);//线程回调函数

class cthread
{
public:
    cthread();
    ~cthread() {};
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
        return (uint32_t)ATOMIC_GET(&threadid);
    };
    
    void _setid(const ATOMIC_T &uiid)
    {
        ATOMIC_SET(&threadid, uiid);
    };
    volatile ATOMIC_T *_getstart()
    {
        return &start;
    };
    ctask *_gettask()
    {
        return task;
    };
    thread_cb _getfunc()
    {
        return funccb;
    };
    void *_getparam()
    {
        return param;
    };
private:
    volatile ATOMIC_T threadid;
    volatile ATOMIC_T start;
    
#ifdef OS_WIN
    HANDLE pthread;
#else
    pthread_t pthread;//线程句柄
#endif
    ctask *task;
    void *param;
    thread_cb funccb;
};

SREY_NS_END

#endif//THREAD_H_
