#include "utils/sfid.h"
#include "utils/utils.h"

#define DefMachineBitLen 10 //机器 ID 默认位数
#define DefSequenceBitLen 12 //自增序列默认位数
#define DefCustomEpoch 1704067200000llu //默认自定义纪元（2024-01-01 00:00:00 UTC 毫秒时间戳）
#define SFID_CLOCKBACK_WAIT 1000 //ctx->clockback_wait 的默认值

// 全部校验走局部量,过了才写 ctx:半初始化的 ctx 拿去 sfid_id 会按垃圾位数移位
sfid_ctx *sfid_init(sfid_ctx *ctx, int32_t machineid, int32_t machinebitlen, int32_t sequencebitlen, uint64_t customepoch) {
    int32_t mbits = 0 == machinebitlen ? DefMachineBitLen : machinebitlen;
    int32_t sbits = 0 == sequencebitlen ? DefSequenceBitLen : sequencebitlen;
    uint64_t epoch = 0 == customepoch ? DefCustomEpoch : customepoch;
    uint64_t curms = nowms();
    if (mbits < 1
        || sbits < 1
        || mbits > 22 - sbits
        || machineid < 0
        || machineid > (int32_t)((1u << mbits) - 1)
        || epoch >= curms) {
        return NULL;
    }
    uint64_t last = curms - epoch;
    if ((last >> (63 - (mbits + sbits))) != 0) {
        return NULL;
    }
    ZERO(ctx, sizeof(sfid_ctx));
    ctx->machinebitlen = mbits;
    ctx->sequencebitlen = sbits;
    ctx->timestampshift = mbits + sbits;
    ctx->machineid = machineid;
    ctx->sequencemask = (1u << sbits) - 1;
    ctx->clockback_wait = SFID_CLOCKBACK_WAIT;
    ctx->customepoch = epoch;
    ctx->lasttimestamp = last;
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
