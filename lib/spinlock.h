#ifndef SPINLOCK_H_
#define SPINLOCK_H_

#include "macro.h"

SREY_NS_BEGIN

class cspinlock
{
public:
    cspinlock();
    ~cspinlock();

    /*
    * \brief          锁定
    */
    void lock();
    /*
    * \brief          非阻塞锁定
    * \return         true 成功
    */
    bool trylock();
    /*
    * \brief          解锁
    */
    void unlock();

private:
#ifdef OS_WIN
    CRITICAL_SECTION spin;
#else
    pthread_spinlock_t spin;
#endif
};

SREY_NS_END

#endif//SPINLOCK_H_
