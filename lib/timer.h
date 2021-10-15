#ifndef TIMER_H_
#define TIMER_H_

#include "macro.h"

SREY_NS_BEGIN

class ctimer
{
public:
    ctimer() 
    {
#if defined(OS_WIN)
        m_interval = INIT_NUMBER;
        LARGE_INTEGER freq;
        ASSERTAB(QueryPerformanceFrequency(&freq), ERRORSTR(ERRNO));
        m_interval = 1.0 / freq.QuadPart;
#elif defined(OS_DARWIN) 
        mach_timebase_info_data_t timebase;
        ASSERTAB(KERN_SUCCESS == mach_timebase_info(&timebase), "mach_timebase_info error.");
        m_interval = (double)timebase.numer / (double)timebase.denom;
        timefunc = (uint64_t(*)(void)) dlsym(RTLD_DEFAULT, "mach_continuous_time");
        if (NULL == timefunc)
        {
            timefunc = mach_absolute_time;
        }
#else
#endif
    };
    ~ctimer() {};

    /*
    * \brief          当前时间
    */
    uint64_t nanosec()
    {
#if defined(OS_WIN)
        LARGE_INTEGER lnow;
        ASSERTAB(QueryPerformanceCounter(&lnow), ERRORSTR(ERRNO));
        return (uint64_t)(lnow.QuadPart * m_interval * NANOSEC);
#elif defined(OS_AIX)
        timebasestruct_t t;
        read_wall_time(&t, TIMEBASE_SZ);
        time_base_to_time(&t, TIMEBASE_SZ);
        return (((uint64_t)t.tb_high) * NANOSEC + t.tb_low);
#elif defined(OS_SUN)
        return gethrtime();
#elif defined(OS_DARWIN)
        return (uint64_t)(timefunc() * m_interval);
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (((uint64_t)ts.tv_sec) * NANOSEC + ts.tv_nsec);
#endif
    };
    /*
    * \brief          开始计时
    */
    void start()
    {
        m_starttick = nanosec();
    };
    /*
    * \brief          结束计时
    * \return         用时 微秒
    */
    uint64_t elapsed()
    {
        return nanosec() - m_starttick;
    };

private:
#if defined(OS_WIN)
    double m_interval;
#elif defined(OS_DARWIN) 
    double m_interval;
    uint64_t(*timefunc)(void); 
#else
#endif
    uint64_t m_starttick;
};

SREY_NS_END

#endif//TIMER_H_
