#ifndef SFID_H_
#define SFID_H_

#include "base/macro.h"

//snowflake id
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

//machinebitlen  sequencebitlen customepoch 为0则取默认值 spin为NULL则不启用锁
sfid_ctx *sfid_init(sfid_ctx *ctx, int32_t machineid,
    int32_t machinebitlen, int32_t sequencebitlen, uint64_t customepoch);
uint64_t sfid_id(sfid_ctx *ctx);
void sfid_decode(sfid_ctx *ctx, uint64_t id, 
    uint64_t *timestamp, int32_t *machineid, int32_t *sequence);

#endif
