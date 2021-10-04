#include "queue.h"
#include "errcode.h"

SREY_NS_BEGIN

cqueue::cqueue(const int32_t &icapacity)
{
    ASSERTAB((icapacity > 0  && icapacity <= INT_MAX / sizeof(void*)), "capacity too large");

    size = INIT_NUMBER;
    next = INIT_NUMBER;
    capacity = icapacity;
    data = new(std::nothrow) void*[capacity];
    ASSERTAB(NULL != data, ERRSTR_MEMORY);
}
cqueue::~cqueue()
{
    SAFE_DELARR(data);
}
bool cqueue::push(void *pval)
{
    if (size >= capacity)
    {
        return false;
    }

    int32_t pos = next + size;
    if (pos >= capacity)
    {
        pos -= capacity;
    }

    data[pos] = pval;
    size++;

    return true;
}
void *cqueue::pop()
{
    void *pval = NULL;

    if (size > INIT_NUMBER)
    {
        pval = data[next];
        next++;
        size--;
        if (next >= capacity)
        {
            next -= capacity;
        }
    }

    return pval;
}
void *cqueue::peek()
{
    return size ? data[next] : NULL;
}

SREY_NS_END
