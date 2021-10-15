#include "chan.h"
#include "utils.h"

SREY_NS_BEGIN

typedef struct select_ctx
{
    bool recv;
    cchan *pchan;
    void *pmsgin;
    int32_t index;
} select_ctx;

cchan::cchan(const int32_t &icapacity)
{
    m_queue = NULL;
    if (icapacity > INIT_NUMBER)
    {
        m_queue = new(std::nothrow) cqueue(icapacity);
        ASSERTAB(NULL != m_queue, ERRSTR_MEMORY);
    }

    m_closed = false;
    m_rwaiting = INIT_NUMBER;
    m_wwaiting = INIT_NUMBER;
    m_data = NULL;

    struct timeval ts;
    timeofday(&ts);
    srand(ts.tv_usec);
}
cchan::~cchan()
{
    SAFE_DEL(m_queue);
}
void cchan::close()
{
    m_mmu.lock();
    if (!m_closed)
    {
        m_closed = true;
        m_rcond.broadcast();
        m_wcond.broadcast();
    }
    m_mmu.unlock();
}
bool cchan::_bufferedsend(void *pdata)
{
    m_mmu.lock();
    while (m_queue->getsize() == m_queue->getcap())
    {
        //队列满 阻塞直到有数据被移除.
        m_wwaiting++;
        m_wcond.wait(&m_mmu);
        m_wwaiting--;
    }

    bool bsuccess = m_queue->push(pdata);

    if (m_rwaiting > INIT_NUMBER)
    {
        //通知可读.
        m_rcond.signal();
    }

    m_mmu.unlock();

    return bsuccess;
}
bool cchan::_bufferedrecv(void **pdata)
{
    m_mmu.lock();
    while (INIT_NUMBER == m_queue->getsize())
    {
        if (m_closed)
        {
            m_mmu.unlock();
            return false;
        }

        //阻塞直到有数据.
        m_rwaiting++;
        m_rcond.wait(&m_mmu);
        m_rwaiting--;
    }

    void *pmsg = m_queue->pop();
    if (NULL != pdata)
    {
        *pdata = pmsg;
    }

    if (m_wwaiting > INIT_NUMBER)
    {
        //通知可写.
        m_wcond.signal();
    }

    m_mmu.unlock();

    return true;
}
bool cchan::_unbufferedsend(void* pdata)
{
    m_wmu.lock();
    m_mmu.lock();

    if (m_closed)
    {
        m_mmu.unlock();
        m_wmu.unlock();
        return false;
    }

    m_data = pdata;
    m_wwaiting++;

    if (m_rwaiting > INIT_NUMBER)
    {
        // 激发读取.
        m_rcond.signal();
    }

    //阻塞直到数据被取出.
    m_wcond.wait(&m_mmu);

    m_mmu.unlock();
    m_wmu.unlock();

    return true;
}
bool cchan::_unbufferedrecv(void **pdata)
{
    m_rmu.lock();
    m_mmu.lock();

    while (!m_closed 
        && INIT_NUMBER == m_wwaiting)
    {
        //阻塞直到有数据.
        m_rwaiting++;
        m_rcond.wait(&m_mmu);
        m_rwaiting--;
    }

    if (m_closed)
    {
        m_mmu.unlock();
        m_rmu.unlock();
        return false;
    }

    if (NULL != pdata)
    {
        *pdata = m_data;
    }
    m_wwaiting--;

    //通知可写.
    m_wcond.signal();

    m_mmu.unlock();
    m_rmu.unlock();

    return true;
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
            stselect.pmsgin = NULL;
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
            stselect.pmsgin = psend_msgs[i];
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
        if (!stselect.pchan->send(stselect.pmsgin))
        {
            SAFE_DELARR(pselect);
            return ERR_FAILED;
        }
    }

    int32_t idex = stselect.index;
    SAFE_DELARR(pselect);

    return idex;
}

SREY_NS_END
