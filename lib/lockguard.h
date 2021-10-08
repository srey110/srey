#ifndef LOCKGUARD_H_
#define LOCKGUARD_H_

#include "macro.h"

SREY_NS_BEGIN

template<class T>
class clockguard
{
public:
    clockguard(T *t) : lock(t) 
    {
        lock->lock();
    }
    ~clockguard() 
    {
        lock->unlock();
    }
protected:
    T *lock;
};

SREY_NS_END

#endif//LOCKGUARD_H_
