#ifndef RWLOCK_H_
#define RWLOCK_H_

#include "macro.h"

SREY_NS_BEGIN

class crwlock
{
public:
    crwlock();
    ~crwlock();

    /*
    * \brief          读锁定
    */
    void rdlock();
    /*
    * \brief          非阻塞读锁定
    * \return         true 成功
    */
    bool tryrdlock();
    /*
    * \brief          写锁定
    * \return         true 成功
    */
    void wrlock();
    /*
    * \brief          非阻塞写锁定
    * \return         true 成功
    */
    bool trywrlock();
    /*
    * \brief          解锁
    */
    void unlock();

private:
#ifdef OS_WIN
    SRWLOCK lock;
    bool exclusive;
#else
    pthread_rwlock_t lock;
#endif
};

SREY_NS_END

#endif//RWLOCK_H_
