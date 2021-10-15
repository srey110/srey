#ifndef CHAN_H_
#define CHAN_H_

#include "queue.h"
#include "mutex.h"
#include "cond.h"

SREY_NS_BEGIN

class cchan
{
public:
    /*
    * \param   icapacity 大于0 带缓存非阻塞
    */
    cchan(const int32_t &icapacity);
    ~cchan();

    /*
    * \brief  关闭channel，关闭后不能写入
    */
    void close();
    /*
    * \brief          channel是否关闭
    * \return         true 已经关闭
    */
    bool isclosed()
    {
        m_mmu.lock();
        bool bclosed = m_closed;
        m_mmu.unlock();

        return bclosed;
    };
    /*
    * \brief          写入数据
    * \param pdata    待写入的数据
    * \return         true 成功
    */
    bool send(void *pdata)
    {
        if (isclosed())
        {
            return false;
        }
        return NULL != m_queue ? _bufferedsend(pdata) : _unbufferedsend(pdata);
    };
    /*
    * \brief          读取数据
    * \param pdata    读取到的数据
    * \return         true 成功
    */
    bool recv(void **pdata)
    {
        return NULL != m_queue ? _bufferedrecv(pdata) : _unbufferedrecv(pdata);
    };
    /*
    * \brief          数据总数
    * \return         数据总数
    */
    int32_t size()
    {
        int32_t uisize = INIT_NUMBER;
        if (NULL != m_queue)
        {
            m_mmu.lock();
            uisize = m_queue->getsize();
            m_mmu.unlock();
        }
        return uisize;
    };
    /*
    * \brief          是否可读
    * \return         true 有数据
    */
    bool canrecv()
    {
        if (NULL != m_queue)
        {
            return size() > INIT_NUMBER;
        }

        m_mmu.lock();
        bool bcanrecv = m_wwaiting > INIT_NUMBER;
        m_mmu.unlock();

        return bcanrecv;
    };
    /*
    * \brief          是否可写
    * \return         true 可写
    */
    bool cansend()
    {
        bool bsend;
        if (NULL != m_queue)
        {
            m_mmu.lock();
            bsend = m_queue->getsize() < m_queue->getcap();
            m_mmu.unlock();
        }
        else
        {
            m_mmu.lock();
            bsend = m_rwaiting > INIT_NUMBER;
            m_mmu.unlock();
        }

        return bsend;
    };
    /*
    * \brief             随机选取一可读写的channel来执行读写,阻塞的不支持
    * \param precv_ctx   读cchan
    * \param irecv_count 读cchan数量
    * \param precv_out   读到的数据
    * \param psend_ctx   写cchan
    * \param isend_count 写cchan数量
    * \param psend_msgs  每个cchan需要发送的数据
    * \return            ERR_FAILED 失败
    * \return            cchan 下标
    */
    static int32_t select(cchan *precv[], const int32_t &irecv_count, void **precv_out,
        cchan *psend[], const int32_t &isend_count, void *psend_msgs[]);

    template <typename T>
    bool sendt(const T &val)
    {
        T *pwrapped = new(std::nothrow) T();
        if (NULL == pwrapped)
        {
            return false;
        }
        *pwrapped = val;

        bool bsuccess = send(pwrapped);
        if (!bsuccess)
        {
            SAFE_DEL(pwrapped);
        }

        return bsuccess;
    };
    template <typename T>
    bool recvt(T &val)
    {
        void *pwrapped = NULL;
        bool bsuccess = recv(&pwrapped);
        if (NULL != pwrapped)
        {
            val = *((T*)pwrapped);
            delete((T *)pwrapped);
        }

        return bsuccess;
    };
    template <typename T>
    bool recvt(T **val)
    {
        return recv((void **)val);
    };

private:
    bool _bufferedsend(void *pdata);
    bool _bufferedrecv(void **pdata);
    bool _unbufferedsend(void* pdata);
    bool _unbufferedrecv(void **pdata);   

private:
    cmutex m_rmu;//读锁
    cmutex m_wmu;//写锁    
    cmutex m_mmu;//读写信号锁
    ccond m_rcond;
    ccond m_wcond;
    bool m_closed;
    int32_t m_rwaiting;
    int32_t m_wwaiting;
    void *m_data;
    cqueue *m_queue;
};

SREY_NS_END

#endif//CHAN_H_
