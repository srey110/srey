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

    centerid = icenterid;
    workid = iworkid;
    sequence = INIT_NUMBER;
    lasttime = nowmsec();
}
uint64_t csnowflake::_untilnextms()
{
    uint64_t ulcur = nowmsec();
    while (ulcur <= lasttime)
    {
        ulcur = nowmsec();
    }

    return ulcur;
}
uint64_t csnowflake::id()
{
    uint64_t uicur = nowmsec();
    ASSERTAB(uicur >= lasttime, "time error.");
    if (uicur == lasttime)
    {
        sequence = (sequence + 1) & SequenceMask;
        if (INIT_NUMBER == sequence)
        {
            //当前毫秒内计数满了，则等待下一秒
            uicur = _untilnextms();
        }
    }
    else
    {
        sequence = INIT_NUMBER;
    }

    lasttime = uicur;

    return ((uint64_t)(uicur - Epoch) << TimestampLeftShift) |
        ((uint64_t)centerid << DatacenterIdShift) |
        ((uint64_t)workid << WorkerIdShift) |
        sequence;
}

SREY_NS_END
