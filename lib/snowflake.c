#include "snowflake.h"
#include "utils.h"

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

void sfid_init(struct sfid_ctx *pctx, const int32_t icenterid, const int32_t iworkid)
{
    ASSERTAB(icenterid <= MaxDatacenterId && icenterid >= 0 
        && iworkid <= MaxWorkerId && iworkid >= 0, "param error.");

    pctx->centerid = icenterid;
    pctx->workid = iworkid;
    pctx->sequence = 0;
    pctx->lasttime = nowmsec();
}
uint64_t _untilnextms(struct sfid_ctx *pctx)
{
    uint64_t ulcur = nowmsec();
    while (ulcur <= pctx->lasttime)
    {
        ulcur = nowmsec();
    }

    return ulcur;
}
uint64_t sfid_id(struct sfid_ctx *pctx)
{
    uint64_t uicur = nowmsec();
    ASSERTAB(uicur >= pctx->lasttime, "time error.");
    if (uicur == pctx->lasttime)
    {
        pctx->sequence = (pctx->sequence + 1) & SequenceMask;
        if (0 == pctx->sequence)
        {
            //当前毫秒内计数满了，则等待下一秒
            uicur = _untilnextms(pctx);
        }
    }
    else
    {
        pctx->sequence = 0;
    }

    pctx->lasttime = uicur;

    return ((uint64_t)(uicur - Epoch) << TimestampLeftShift) |
        ((uint64_t)pctx->centerid << DatacenterIdShift) |
        ((uint64_t)pctx->workid << WorkerIdShift) |
        pctx->sequence;
}
