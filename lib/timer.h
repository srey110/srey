#ifndef TIMER_H_
#define TIMER_H_

#include "utils.h"

SREY_NS_BEGIN

class ctimer
{
public:
    ctimer() {};
    ~ctimer() {};

    /*
    * \brief          开始计时
    */
    void start()
    {
        timeofday(&starttime);
    }
    /*
    * \brief          结束计时
    * \return         用时 微秒
    */
    uint64_t elapsed()
    {
        timeofday(&endtime);
        return (endtime.tv_sec - starttime.tv_sec) * 1000000 + (endtime.tv_usec - starttime.tv_usec);
    }

private:
    struct timeval starttime;
    struct timeval endtime;
};

SREY_NS_END

#endif//TIMER_H_
