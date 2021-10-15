#include "ev.h"
#include "timer.h"

SREY_NS_BEGIN

void cev::beforrun()
{
    m_wot.init();
}
void cev::run()
{
    while (!isstop())
    {
        m_wot.run();
    }
}
void cev::afterrun()
{
    m_wot.stop();
}

SREY_NS_END
