#include "utils/sfid.h"
#include "utils/utils.h"

#define DefMachineBitLen 10
#define DefSequenceBitLen 12
#define DefCustomEpoch 1704067200000llu

sfid_ctx *sfid_init(sfid_ctx *ctx, int32_t machineid, int32_t machinebitlen, int32_t sequencebitlen, uint64_t customepoch) {
    ctx->machineid = machineid;
    ctx->machinebitlen = 0 == machinebitlen ? DefMachineBitLen : machinebitlen;
    ctx->sequencebitlen = 0 == sequencebitlen ? DefSequenceBitLen : sequencebitlen;
    ctx->customepoch = 0 == customepoch ? DefCustomEpoch : customepoch;
    int32_t maxmachineid = (1 << ctx->machinebitlen) - 1;
    uint64_t curms = nowms();
    if (ctx->sequencebitlen < 1
        || ctx->machinebitlen + ctx->sequencebitlen > 22
        || ctx->machineid < 0
        || ctx->machineid > maxmachineid
        || ctx->customepoch >= curms) {
        return NULL;
    }
    ctx->lasttimestamp = curms - ctx->customepoch;
    ctx->sequence = 0;
    ctx->sequencemask = (1 << ctx->sequencebitlen) - 1;
    ctx->timestampshift = ctx->machinebitlen + ctx->sequencebitlen;
    ctx->machineidshift = ctx->sequencebitlen;
    return ctx;
}
uint64_t sfid_id(sfid_ctx *ctx) {
    uint64_t id, curms;
    while (1) {
        curms = nowms() - ctx->customepoch;
        if (curms < ctx->lasttimestamp) {
            continue;
        } else if (curms == ctx->lasttimestamp) {
            if (ctx->sequence >= ctx->sequencemask) {
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
