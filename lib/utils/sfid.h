#ifndef SFID_H_
#define SFID_H_

#include "base/macro.h"

typedef struct sfid_ctx {
    int32_t machinebitlen;   //机器 ID 占用的位数
    int32_t sequencebitlen;  //自增序列占用的位数
    int32_t timestampshift;  //时间戳左移位数（= machinebitlen + sequencebitlen）
    int32_t machineidshift;  //机器 ID 左移位数（= sequencebitlen）
    int32_t sequence;        //当前自增序列值
    int32_t machineid;       //机器 ID
    int32_t sequencemask;    //自增序列掩码（用于回绕检测）
    uint64_t customepoch;    //自定义纪元时间戳（毫秒），ID 中的时间戳相对于此值
    uint64_t lasttimestamp;  //上次生成 ID 时的时间戳（相对于 customepoch 的毫秒数）
}sfid_ctx;
/// <summary>
/// snowflake id 初始化
/// </summary>
/// <param name="ctx">sfid_ctx</param>
/// <param name="machineid">
///   机器ID，范围 [0, 2^machinebitlen - 1]。
///   默认 machinebitlen=10 时最大值为 1023。
/// </param>
/// <param name="machinebitlen">
///   机器ID占用位数，0 使用默认值 10。
///   合理范围 [1, 20]；machinebitlen + sequencebitlen 必须等于 22。
///   位数越大可支持的机器节点越多（2^machinebitlen 台），
///   但同一毫秒内可生成的 ID 数（2^sequencebitlen）相应减少。
/// </param>
/// <param name="sequencebitlen">
///   自增序列占用位数，0 使用默认值 12。
///   合理范围 [2, 21]；machinebitlen + sequencebitlen 必须等于 22。
///   位数越大每毫秒可生成的 ID 越多（2^sequencebitlen 个/ms），
///   建议不低于 7（即 128 个/ms）以保证足够吞吐量。
/// </param>
/// <param name="customepoch">
///   自定义纪元时间戳（毫秒），ID 中的时间戳字段相对于此值计算，
///   可延长可用年限。0 使用默认值（2024-01-01 00:00:00 UTC）。
///   必须小于当前时间，否则返回 NULL。
/// </param>
/// <returns>成功返回 ctx，参数非法返回 NULL</returns>
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
