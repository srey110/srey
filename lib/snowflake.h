#ifndef SNOWFLAKE_H_
#define SNOWFLAKE_H_

#include "macro.h"

SREY_NS_BEGIN

class csnowflake
{
public:
    csnowflake(const int32_t &icenterid, const int32_t &iworkid);
    ~csnowflake() {};
    /*
    * \brief          Éú³Éid
    * \return         id
    */
    uint64_t id();

private:
    uint64_t _untilnextms();

private:
    int32_t workid;
    int32_t centerid;
    long sequence;
    uint64_t lasttime;
};

SREY_NS_END

#endif//SNOWFLAKE_H_
