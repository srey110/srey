#include "chan.h"
#include "utils.h"
#include "errcode.h"

SREY_NS_BEGIN

typedef struct select_ctx
{
    bool recv;
    cchan *pchan;
    void *pmsg_in;
    int32_t index;
} select_ctx;

cchan::cchan(const int32_t &icapacity)
{
    queue = NULL;
    if (icapacity > INIT_NUMBER)
    {
        queue = new(std::nothrow) cqueue(icapacity);
        ASSERTAB(NULL != queue, ERRSTR_MEMORY);
    }

    closed = false;
    r_waiting = INIT_NUMBER;
    w_waiting = INIT_NUMBER;
    data = NULL;

    struct timeval ts;
    timeofday(&ts);
    srand(ts.tv_usec);
}
cchan::~cchan()
{
    SAFE_DEL(queue);
}
void cchan::close()
{
    m_mu.lock();
    if (!closed)
    {
        closed = true;
        r_cond.broadcast();
        w_cond.broadcast();
    }
    m_mu.unlock();
}
bool cchan::isclosed()
{
    m_mu.lock();
    bool bclosed = closed;
    m_mu.unlock();

    return bclosed;
}
bool cchan::_bufferedsend(void *pdata)
{
    m_mu.lock();
    while (queue->getsize() == queue->getcap())
    {
        //队列满 阻塞直到有数据被移除.
        w_waiting++;
        w_cond.wait(&m_mu);
        w_waiting--;
    }

    bool bsuccess = queue->push(pdata);

    if (r_waiting > INIT_NUMBER)
    {
        //通知可读.
        r_cond.signal();
    }

    m_mu.unlock();

    return bsuccess;
}
bool cchan::_bufferedrecv(void **pdata)
{
    m_mu.lock();
    while (INIT_NUMBER == queue->getsize())
    {
        if (closed)
        {
            m_mu.unlock();
            return false;
        }

        //阻塞直到有数据.
        r_waiting++;
        r_cond.wait(&m_mu);
        r_waiting--;
    }

    void *pmsg = queue->pop();
    if (NULL != pdata)
    {
        *pdata = pmsg;
    }

    if (w_waiting > INIT_NUMBER)
    {
        //通知可写.
        w_cond.signal();
    }

    m_mu.unlock();

    return true;
}
bool cchan::_unbufferedsend(void* pdata)
{
    w_mu.lock();
    m_mu.lock();

    if (closed)
    {
        m_mu.unlock();
        w_mu.unlock();
        return false;
    }

    data = pdata;
    w_waiting++;

    if (r_waiting > INIT_NUMBER)
    {
        // 激发读取.
        r_cond.signal();
    }

    //阻塞直到数据被取出.
    w_cond.wait(&m_mu);

    m_mu.unlock();
    w_mu.unlock();

    return true;
}
bool cchan::_unbufferedrecv(void **pdata)
{
    r_mu.lock();
    m_mu.lock();

    while (!closed 
        && INIT_NUMBER == w_waiting)
    {
        //阻塞直到有数据.
        r_waiting++;
        r_cond.wait(&m_mu);
        r_waiting--;
    }

    if (closed)
    {
        m_mu.unlock();
        r_mu.unlock();
        return false;
    }

    if (NULL != pdata)
    {
        *pdata = data;
    }
    w_waiting--;

    //通知可写.
    w_cond.signal();

    m_mu.unlock();
    r_mu.unlock();

    return true;
}
bool cchan::send(void *pdata)
{
    if (isclosed())
    {
        return false;
    }

    return NULL != queue ? _bufferedsend(pdata) : _unbufferedsend(pdata);
}
bool cchan::recv(void **pdata)
{
    return NULL != queue ? _bufferedrecv(pdata) : _unbufferedrecv(pdata);
}
int32_t cchan::size()
{
    int32_t size = INIT_NUMBER;
    if (NULL != queue)
    {
        m_mu.lock();
        size = queue->getsize();
        m_mu.unlock();
    }

    return size;
}
bool cchan::canrecv()
{
    if (NULL != queue)
    {
        return size() > INIT_NUMBER;
    }

    m_mu.lock();
    bool bcanrecv = w_waiting > INIT_NUMBER;
    m_mu.unlock();

    return bcanrecv;
}
bool cchan::cansend()
{
    bool bsend;
    if (NULL != queue)
    {
        m_mu.lock();
        bsend = queue->getsize() < queue->getcap();
        m_mu.unlock();
    }
    else
    {
        m_mu.lock();
        bsend = r_waiting > INIT_NUMBER;
        m_mu.unlock();
    }

    return bsend;
}
int32_t cchan::select(cchan *precv[], const int32_t &irecv_count, void **precv_out,
    cchan *psend[], const int32_t &isend_count, void *psend_msgs[])
{
    select_ctx *pselect = new(std::nothrow) select_ctx[irecv_count + isend_count];
    if (NULL == pselect)
    {
        return ERR_FAILED;
    }

    int32_t i;
    int32_t icount = INIT_NUMBER;
    for (i = 0; i < irecv_count; i++)
    {
        cchan* pchan = precv[i];
        if (pchan->canrecv())
        {
            select_ctx stselect;
            stselect.recv = true;
            stselect.pchan = pchan;
            stselect.pmsg_in = NULL;
            stselect.index = i;
            pselect[icount++] = stselect;
        }
    }
    for (i = 0; i < isend_count; i++)
    {
        cchan* pchan = psend[i];
        if (pchan->cansend())
        {
            select_ctx stselect;
            stselect.recv = false;
            stselect.pchan = pchan;
            stselect.pmsg_in = psend_msgs[i];
            stselect.index = i + irecv_count;
            pselect[icount++] = stselect;
        }
    }

    if (INIT_NUMBER == icount)
    {
        SAFE_DELARR(pselect);
        return ERR_FAILED;
    }

    select_ctx stselect = pselect[rand() % icount];
    if (stselect.recv)
    {
        if (!stselect.pchan->recv(precv_out))
        {
            SAFE_DELARR(pselect);
            return ERR_FAILED;
        }
    }
    else
    {
        if (!stselect.pchan->send(stselect.pmsg_in))
        {
            SAFE_DELARR(pselect);
            return ERR_FAILED;
        }
    }

    int32_t idex = stselect.index;
    SAFE_DELARR(pselect);

    return idex;
}
bool cchan::send(const int32_t &ival)
{
    int32_t *pwrapped = new(std::nothrow) int32_t();
    if (NULL == pwrapped)
    {
        return false;
    }

    *pwrapped = ival;

    bool bsuccess = send(pwrapped);
    if (!bsuccess)
    {
        SAFE_DEL(pwrapped);
    }

    return bsuccess;
}
bool cchan::recv(int32_t *pval)
{
    void *pwrapped = NULL;
    bool bsuccess = recv(&pwrapped);
    if (NULL != pwrapped)
    {
        *pval = *(int32_t *)pwrapped;
        delete((int32_t *)pwrapped);
    }

    return bsuccess;
}
bool cchan::send(const int64_t &ival)
{
    int64_t *pwrapped = new(std::nothrow) int64_t();
    if (NULL == pwrapped)
    {
        return false;
    }

    *pwrapped = ival;

    bool bsuccess = send(pwrapped);
    if (!bsuccess)
    {
        SAFE_DEL(pwrapped);
    }

    return bsuccess;
}
bool cchan::recv(int64_t *pval)
{
    void *pwrapped = NULL;
    bool bsuccess = recv(&pwrapped);
    if (NULL != pwrapped)
    {
        *pval = *(int64_t *)pwrapped;
        delete((int64_t *)pwrapped);
    }

    return bsuccess;
}
bool cchan::send(const double &dval)
{
    double *pwrapped = new(std::nothrow) double();
    if (NULL == pwrapped)
    {
        return false;
    }

    *pwrapped = dval;

    bool bsuccess = send(pwrapped);
    if (!bsuccess)
    {
        SAFE_DEL(pwrapped);
    }

    return bsuccess;
}
bool cchan::recv(double *pval)
{
    void *pwrapped = NULL;
    bool bsuccess = recv(&pwrapped);
    if (NULL != pwrapped)
    {
        *pval = *(double *)pwrapped;
        delete((double *)pwrapped);
    }

    return bsuccess;
}
bool cchan::send(const void *pval, const size_t &isize)
{
    std::string *pbuf = new(std::nothrow) std::string();
    if (NULL == pbuf)
    {
        return false;
    }

    pbuf->append((const char*)pval, isize);

    bool bsuccess = send(pbuf);
    if (!bsuccess)
    {
        SAFE_DEL(pbuf);
    }

    return bsuccess;
}
bool cchan::recv(std::string *pbuf)
{
    void *pwrapped = NULL;
    bool bsuccess = recv(&pwrapped);
    if (NULL != pwrapped)
    {
        *pbuf = *(std::string *)pwrapped;
        delete((std::string *)pwrapped);
    }

    return bsuccess;
}

SREY_NS_END
