#ifndef QUEUE_H_
#define QUEUE_H_

#include "macro.h"

SREY_NS_BEGIN

class cqueue
{
public:
    cqueue(const int32_t &icapacity);
    ~cqueue();

    /*
    * \brief          添加数据
    * \param pval     需要添加的数据
    * \return         true 成功
    */
    bool push(void *pval);
    /*
    * \brief          弹出一数据
    * \return         NULL 无数据
    * \return         数据
    */
    void *pop();
    /*
    * \brief          获取第一个数据
    * \return         NULL 无数据
    * \return         数据
    */
    void *peek();
    const int32_t &getsize()
    {
        return size;
    };
    const int32_t &getcap()
    {
        return capacity;
    };

private:
    int32_t size;
    int32_t next;
    int32_t capacity;
    void **data;
};

SREY_NS_END

#endif//QUEUE_H_
