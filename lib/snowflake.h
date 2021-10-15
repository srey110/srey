#ifndef SNOWFLAKE_H_
#define SNOWFLAKE_H_

#include "macro.h"

SREY_NS_BEGIN

class csnowflake
{
public:
    explicit csnowflake(const int32_t &icenterid, const int32_t &iworkid);
    ~csnowflake() {};
    /*
    * \brief          Éú³Éid
    * \return         id
    */
    uint64_t id();

private:
    uint64_t _untilnextms();

private:
    int32_t m_workid;
    int32_t m_centerid;
    long m_sequence;
    uint64_t m_lasttime;
};

SREY_NS_END

#endif//SNOWFLAKE_H_
