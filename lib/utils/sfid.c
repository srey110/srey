#include "utils/sfid.h"
#include "utils/utils.h"

#define DefMachineBitLen 10      //机器 ID 默认位数
#define DefSequenceBitLen 12     //自增序列默认位数
#define DefCustomEpoch 1704067200000llu //默认自定义纪元（2024-01-01 00:00:00 UTC 毫秒时间戳）

sfid_ctx *sfid_init(sfid_ctx *ctx, int32_t machineid, int32_t machinebitlen, int32_t sequencebitlen, uint64_t customepoch) {
    ctx->machineid = machineid;
    ctx->machinebitlen = 0 == machinebitlen ? DefMachineBitLen : machinebitlen;
    ctx->sequencebitlen = 0 == sequencebitlen ? DefSequenceBitLen : sequencebitlen;
    ctx->customepoch = 0 == customepoch ? DefCustomEpoch : customepoch;
    uint64_t curms = nowms();
    // 先做范围校验，再计算派生值，避免位移溢出
    if (ctx->machinebitlen < 1
        || ctx->sequencebitlen < 1
        || ctx->machinebitlen + ctx->sequencebitlen > 22
        || ctx->machineid < 0
        || ctx->machineid > (int32_t)((1u << ctx->machinebitlen) - 1)
        || ctx->customepoch >= curms) {
        return NULL;
    }
    ctx->lasttimestamp = curms - ctx->customepoch;
    ctx->sequence = 0;
    ctx->sequencemask = (1u << ctx->sequencebitlen) - 1;
    ctx->timestampshift = ctx->machinebitlen + ctx->sequencebitlen;
    ctx->machineidshift = ctx->sequencebitlen;
    return ctx;
}
uint64_t sfid_id(sfid_ctx *ctx) {
    uint64_t id, curms;
    for (;;) {
        curms = nowms() - ctx->customepoch;
        if (curms < ctx->lasttimestamp) {
            continue;
        } else if (curms == ctx->lasttimestamp) {
            if (ctx->sequence >= ctx->sequencemask) {
                MSLEEP(1); // 序列号耗尽，等待下一毫秒，避免 busy loop 烧 CPU
                continue;
            } else {
                ctx->sequence++;
                break;
            }
        } else {
            ctx->sequence = 0;
            ctx->lasttimestamp = curms;
            break;
        }
    }
    id = (ctx->lasttimestamp << ctx->timestampshift) |
        ((uint64_t)ctx->machineid << ctx->machineidshift) |
        (ctx->sequence & ctx->sequencemask);
    return id;
}
void sfid_decode(sfid_ctx *ctx, uint64_t id, uint64_t *timestamp, int32_t *machineid, int32_t *sequence) {
    uint64_t timestampmask = (1llu << (63 - ctx->timestampshift)) - 1;
    uint64_t machineidmask = (1llu << ctx->machinebitlen) - 1;
    uint64_t sequencemask = (1llu << ctx->sequencebitlen) - 1;
    *timestamp = ((id >> ctx->timestampshift) & timestampmask) + ctx->customepoch;
    *machineid = (uint32_t)((id >> ctx->machineidshift) & machineidmask);
    *sequence = (uint32_t)(id & sequencemask);
}
