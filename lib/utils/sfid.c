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
    ctx->clockback_warned = 0;
    if ((ctx->lasttimestamp >> (63 - ctx->timestampshift)) != 0) {
        return NULL;
    }
    return ctx;
}
/* sfid_id 无锁设计：每个线程持有独立的 sfid_ctx，禁止多线程共享同一 ctx。
 * lasttimestamp/sequence 字段未加原子保护，属于有意为之——调用方保证单线程访问。*/
uint64_t sfid_id(sfid_ctx *ctx) {
    uint64_t id, curms;
    for (;;) {
        curms = nowms() - ctx->customepoch;
        if (curms < ctx->lasttimestamp) {
            // 时钟回拨：仅首次打印错误，避免高频日志
            if (0 == ctx->clockback_warned) {
                LOG_ERROR("clock rollback detected: cur=%"PRIu64" last=%"PRIu64" diff=%"PRIu64"ms.",
                          curms, ctx->lasttimestamp, ctx->lasttimestamp - curms);
                ctx->clockback_warned = 1;
            }
            MSLEEP(1); // 让出 CPU 等待时钟追上 lasttimestamp，避免 busy loop 烧 CPU
            continue;
        } else if (curms == ctx->lasttimestamp) {
            if (ctx->sequence >= ctx->sequencemask) {
                // 序列号耗尽：自旋等待 ms 跳跃，最长 < 1ms，避免 MSLEEP 在粗粒度时钟下反复短睡眠
                do {
                    CPU_PAUSE();
                    curms = nowms() - ctx->customepoch;
                } while (curms <= ctx->lasttimestamp);
                ctx->sequence = 0;
                ctx->lasttimestamp = curms;
                break;
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
    // 时钟回拨 统一清零标志
    ctx->clockback_warned = 0;
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
