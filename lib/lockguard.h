#ifndef LOCKGUARD_H_
#define LOCKGUARD_H_

#include "macro.h"

SREY_NS_BEGIN

template<class T>
class clockguard
{
public:
    clockguard(T *t, const bool &block = true) : m_block(block), m_lock(t)
    {
        if (m_block)
        {
            m_lock->lock();
        }        
    }
    ~clockguard() 
    {
        if (m_block)
        {
            m_lock->unlock();
        }        
    }
protected:
    bool m_block;
    T *m_lock;
};

SREY_NS_END

#endif//LOCKGUARD_H_
