#ifndef MUTEX_H_
#define MUTEX_H_

#include "macro.h"

SREY_NS_BEGIN

class cmutex
{
public:
    cmutex();
    ~cmutex();

    /*
    * \brief          Ëø¶¨
    */
    void lock();
    /*
    * \brief          ½âËø
    */
    void unlock();

    pthread_mutex_t *getmutex()
    {
        return &mutex;
    };

private:
    pthread_mutex_t mutex;
};

SREY_NS_END

#endif//MUTEX_H_
