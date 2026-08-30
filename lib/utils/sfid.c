#include "utils/sfid.h"
#include "utils/utils.h"

#define DefMachineBitLen 10 //机器 ID 默认位数
#define DefSequenceBitLen 12 //自增序列默认位数
#define DefCustomEpoch 1704067200000llu //默认自定义纪元（2024-01-01 00:00:00 UTC 毫秒时间戳）
#define SFID_CLOCKBACK_WAIT 1000 //ctx->clockback_wait 的默认值

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
    ctx->clockback_warned = 0;
    ctx->clockback_wait = SFID_CLOCKBACK_WAIT;
    if ((ctx->lasttimestamp >> (63 - ctx->timestampshift)) != 0) {
        return NULL;
    }
    return ctx;
}
/* sfid_id 无锁设计：每个线程持有独立的 sfid_ctx，禁止多线程共享同一 ctx。
 * lasttimestamp/sequence 字段未加原子保护，属于有意为之——调用方保证单线程访问。*/
// 时钟回拨时退避一格：首次打印告警，睡 1ms 等时钟追上来。
// 超过 deadline 返 ERR_FAILED 让调用方放弃——回拨多少就阻塞多少的话，一次跳表能把线程卡住几十分钟。
// deadline 按时刻算而不是数睡眠次数：MSLEEP(1) 的实际粒度各平台差着十几倍
static int32_t _sfid_clockback_wait(sfid_ctx *ctx, uint64_t curms, uint64_t *deadline) {
    if (0 == ctx->clockback_warned) {
        LOG_ERROR("clock rollback detected: cur=%"PRIu64" last=%"PRIu64" diff=%"PRIu64"ms.",
                  curms, ctx->lasttimestamp, ctx->lasttimestamp - curms);
        ctx->clockback_warned = 1;
    }
    if (0 == *deadline) {
        *deadline = nowms() + (uint64_t)ctx->clockback_wait;
    }
    if (nowms() >= *deadline) {
        LOG_ERROR("clock rollback exceeds %dms, give up.", ctx->clockback_wait);
        ctx->clockback_warned = 0;
        return ERR_FAILED;
    }
    MSLEEP(1);
    return ERR_OK;
}
uint64_t sfid_id(sfid_ctx *ctx) {
    uint64_t id, curms, now;
    uint64_t deadline = 0;
    for (;;) {
        now = nowms();
        // 墙钟退到 customepoch 之前:无符号减法会下溢成天文数字并永久写进 lasttimestamp,
        // 钳到 0 即落进下面的回拨分支
        curms = (now >= ctx->customepoch) ? now - ctx->customepoch : 0;
        if (curms < ctx->lasttimestamp) {
            if (ERR_OK != _sfid_clockback_wait(ctx, curms, &deadline)) {
                return 0;
            }
            continue;
        } else if (curms == ctx->lasttimestamp) {
            if (ctx->sequence >= ctx->sequencemask) {
                // 序列号耗尽：自旋等 ms 跳变（正常单调时钟下 < 1ms），比 MSLEEP 在粗粒度时钟上
                // 反复短睡强。回外层重判，ms 跳变与时钟回拨两条出口都复用外层已有的分支
                CPU_PAUSE();
                continue;
            }
            ctx->sequence++;
            break;
        } else {
            ctx->sequence = 0;
            ctx->lasttimestamp = curms;
            break;
        }
    }
    // 时钟回拨 统一清零标志
    ctx->clockback_warned = 0;
    id = (ctx->lasttimestamp << ctx->timestampshift) |
        ((uint64_t)ctx->machineid << ctx->sequencebitlen) |
        (ctx->sequence & ctx->sequencemask);
    return id;
}
void sfid_decode(sfid_ctx *ctx, uint64_t id, uint64_t *timestamp, int32_t *machineid, int32_t *sequence) {
    uint64_t timestampmask = (1llu << (63 - ctx->timestampshift)) - 1;
    uint64_t machineidmask = (1llu << ctx->machinebitlen) - 1;
    *timestamp = ((id >> ctx->timestampshift) & timestampmask) + ctx->customepoch;
    *machineid = (uint32_t)((id >> ctx->sequencebitlen) & machineidmask);
    *sequence = (uint32_t)(id & (uint64_t)ctx->sequencemask);
}
