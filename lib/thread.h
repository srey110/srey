#ifndef THREAD_H_
#define THREAD_H_

#include "macro.h"

SREY_NS_BEGIN

class ctask
{
public:
    ctask(){};
    ~ctask() {};
    virtual void beforrun() {};
    virtual void run() {};
    virtual void afterrun() {};
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
    * \brief          获取当前状态
    */
    uint32_t state();
    /*
    * \brief          获取、设置启动的线程id
    */
    void setid(const uint32_t &uiid)
    {
        ATOMIC_SET(&threadid, uiid);
    };
    uint32_t getid()
    {
        return ATOMIC_GET(&threadid);
    };
    uint32_t *getstart()
    {
        return &start;
    };
    ctask *gettask()
    {
        return task;
    };
    thread_cb getfunc()
    {
        return funccb;
    };
    void *getparam()
    {
        return param;
    };
private:
    uint32_t threadid;
    uint32_t start;
    
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
