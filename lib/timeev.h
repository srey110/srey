#ifndef TIMEEV_H_
#define TIMEEV_H_

#include "wot.h"
#include "thread.h"

SREY_NS_BEGIN

class ctimeev : public ctask
{
public:
    ctimeev()
    {};
    ~ctimeev()
    {};
    void beforrun()
    {
        m_wot.init();
    };
    void run()
    {
        while (!isstop())
        {
            m_wot.run();
        }
    };
    void afterrun()
    {
        m_wot.stop();
    };
    bool addtimer(class cchan *pchan, const uint32_t &uimsec, const void *pdata)
    {
        return m_wot.add(pchan, uimsec, pdata);
    };

private:
    cwot m_wot;
};

SREY_NS_END

#endif//TIMEEV_H_
