#ifndef COND_H_
#define COND_H_

#include "mutex.h"

SREY_NS_BEGIN

class ccond
{
public:
    ccond();
    ~ccond();
    /*
    * \brief          等待信号量
    */
    void wait(cmutex *pmu);
    /*
    * \brief          等待信号量
    * \param uims     等待时间  毫秒
    */
    void timedwait(cmutex *pmu, const uint32_t &uims);
    /*
    * \brief          激活信号
    */
    void signal();
    /*
    * \brief          激活所有信号
    */
    void broadcast();

private:
#ifdef OS_WIN
    CONDITION_VARIABLE cond;
#else
    pthread_cond_t cond;
#endif
};

SREY_NS_END

#endif//COND_H_
