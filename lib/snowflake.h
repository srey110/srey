#ifndef SNOWFLAKE_H_
#define SNOWFLAKE_H_

#include "macro.h"

struct sfid_ctx
{
    int32_t workid;
    int32_t centerid;
    long sequence;
    uint64_t lasttime;
};
/*
* \brief          初始化
*/
void sfid_init(struct sfid_ctx *pctx, const int32_t icenterid, const int32_t iworkid);
/*
* \brief          生成id
* \return         id
*/
uint64_t sfid_id(struct sfid_ctx *pctx);

#endif//SNOWFLAKE_H_
