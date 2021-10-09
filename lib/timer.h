#ifndef TIMER_H_
#define TIMER_H_

#include "macro.h"

SREY_NS_BEGIN

class ctimer
{
public:
    ctimer();
    ~ctimer() {};

    /*
    * \brief          当前时间
    */
    uint64_t nanosec();
    /*
    * \brief          开始计时
    */
    void start();
    /*
    * \brief          结束计时
    * \return         用时 微秒
    */
    uint64_t elapsed();

private:
#ifdef OS_WIN
    double interval;
#endif
    uint64_t starttick;
};

SREY_NS_END

#endif//TIMER_H_
