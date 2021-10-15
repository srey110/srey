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
        ASSERTAB(NULL == m_instance, "multiple initialization");
        m_instance = static_cast<T*>(this);
    };
    virtual ~csingleton(){ };
    static T* getinstance()
    {
        return m_instance;
    };

private:
    DISALLOWCOPY(csingleton);
    static T *m_instance;
};

#define SINGLETON_INIT(T)							\
    template <>	 T * csingleton<T>::m_instance = NULL;	\

SREY_NS_END

#endif//SINGLETON_H_
