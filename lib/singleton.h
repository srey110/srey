#ifndef SINGLETON_H_
#define SINGLETON_H_

#include "macro.h"

SREY_NS_BEGIN

//¼òµ¥µ¥Àý
template <typename T>
class csingleton
{
public:
    csingleton()
    {
        assert(NULL == instance);
        instance = static_cast<T*>(this);
    };
    virtual ~csingleton(){ };
    static T* getinstance()
    {
        return instance;
    };

private:
    DISALLOWCOPY(csingleton);
    static T *instance;
};

#define SINGLETON_INIT(T)							\
    template <>	 T * csingleton<T>::instance = NULL;	\

SREY_NS_END

#endif//SINGLETON_H_
