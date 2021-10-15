#include "snowflake.h"
#include "utils.h"

SREY_NS_BEGIN

#define Epoch 1358902800000LL
//机器标识位数
#define WorkerIdBits 5
//数据中心标识位数
#define DatacenterIdBits 5
//机器ID最大值
#define MaxWorkerId  (-1 ^ (-1 << WorkerIdBits))
//数据中心ID最大值
#define MaxDatacenterId (-1 ^ (-1 << DatacenterIdBits))
//毫秒内自增位
#define SequenceBits 12
//机器ID偏左移12位
#define WorkerIdShift SequenceBits
//数据中心ID左移17位
#define DatacenterIdShift (SequenceBits + WorkerIdBits)
//时间毫秒左移22位
#define TimestampLeftShift (SequenceBits + WorkerIdBits + DatacenterIdBits)
#define SequenceMask (-1 ^ (-1 << SequenceBits))

csnowflake::csnowflake(const int32_t &icenterid, const int32_t &iworkid)
{
    ASSERTAB(icenterid <= MaxDatacenterId && icenterid >= 0 
        && iworkid <= MaxWorkerId && iworkid >= 0, "param error.");

    m_centerid = icenterid;
    m_workid = iworkid;
    m_sequence = INIT_NUMBER;
    m_lasttime = nowmsec();
}
uint64_t csnowflake::_untilnextms()
{
    uint64_t ulcur = nowmsec();
    while (ulcur <= m_lasttime)
    {
        ulcur = nowmsec();
    }

    return ulcur;
}
uint64_t csnowflake::id()
{
    uint64_t uicur = nowmsec();
    ASSERTAB(uicur >= m_lasttime, "time error.");
    if (uicur == m_lasttime)
    {
        m_sequence = (m_sequence + 1) & SequenceMask;
        if (INIT_NUMBER == m_sequence)
        {
            //当前毫秒内计数满了，则等待下一秒
            uicur = _untilnextms();
        }
    }
    else
    {
        m_sequence = INIT_NUMBER;
    }

    m_lasttime = uicur;

    return ((uint64_t)(uicur - Epoch) << TimestampLeftShift) |
        ((uint64_t)m_centerid << DatacenterIdShift) |
        ((uint64_t)m_workid << WorkerIdShift) |
        m_sequence;
}

SREY_NS_END
