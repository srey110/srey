#ifndef LOCKGUARD_H_
#define LOCKGUARD_H_

#include "macro.h"

SREY_NS_BEGIN

template<class T>
class clockguard
{
public:
    clockguard(T *t, const bool &block = true) : m_block(block), lock(t)
    {
        if (m_block)
        {
            lock->lock();
        }        
    }
    ~clockguard() 
    {
        if (m_block)
        {
            lock->unlock();
        }        
    }
protected:
    bool m_block;
    T *lock;
};

SREY_NS_END

#endif//LOCKGUARD_H_
