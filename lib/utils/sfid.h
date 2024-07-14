#ifndef SFID_H_
#define SFID_H_

#include "base/macro.h"

typedef struct sfid_ctx {
    int32_t machinebitlen;//机器id位数
    int32_t sequencebitlen;//自增序列位数
    int32_t timestampshift;
    int32_t machineidshift;
    int32_t sequence;//自增序列
    int32_t machineid;//机器Id
    int32_t sequencemask;
    uint64_t customepoch;
    uint64_t lasttimestamp;
}sfid_ctx;
/// <summary>
/// snowflake id 初始化
/// </summary>
/// <param name="ctx">sfid_ctx</param>
/// <param name="machineid">机器ID</param>
/// <param name="machinebitlen">机器ID位数, 0 默认10</param>
/// <param name="sequencebitlen">自增序列位数, 0 默认12</param>
/// <param name="customepoch">固定减少, 0 默认</param>
/// <returns>NULL失败</returns>
sfid_ctx *sfid_init(sfid_ctx *ctx, int32_t machineid, int32_t machinebitlen, int32_t sequencebitlen, uint64_t customepoch);
/// <summary>
/// 获取ID
/// </summary>
/// <param name="ctx">sfid_ctx</param>
/// <returns>snowflake id</returns>
uint64_t sfid_id(sfid_ctx *ctx);
/// <summary>
/// 通过ID解析出 时间戳 机器ID 自增序列
/// </summary>
/// <param name="ctx">sfid_ctx</param>
/// <param name="id">snowflake id</param>
/// <param name="timestamp">时间戳 毫秒</param>
/// <param name="machineid">机器ID</param>
/// <param name="sequence">自增序列</param>
void sfid_decode(sfid_ctx *ctx, uint64_t id, uint64_t *timestamp, int32_t *machineid, int32_t *sequence);

#endif
