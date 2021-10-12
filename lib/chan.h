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
    bool isclosed();
    /*
    * \brief          写入数据
    * \param pdata    待写入的数据
    * \return         true 成功
    */
    bool send(void *pdata);
    /*
    * \brief          读取数据
    * \param pdata    读取到的数据
    * \return         true 成功
    */
    bool recv(void **pdata);
    /*
    * \brief          数据总数
    * \return         数据总数
    */
    int32_t size();
    /*
    * \brief          是否可读
    * \return         true 有数据
    */
    bool canrecv();
    /*
    * \brief          是否可写
    * \return         true 可写
    */
    bool cansend();
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

private:
    bool _bufferedsend(void *pdata);
    bool _bufferedrecv(void **pdata);
    bool _unbufferedsend(void* pdata);
    bool _unbufferedrecv(void **pdata);   

private:
    cmutex r_mu;//读锁
    cmutex w_mu;//写锁    
    cmutex m_mu;//读写信号锁
    ccond r_cond;
    ccond w_cond;
    bool closed;
    int32_t r_waiting;
    int32_t w_waiting;
    void *data;
    cqueue *queue;
};

SREY_NS_END

#endif//CHAN_H_
