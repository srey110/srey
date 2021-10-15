#ifndef WOT_H_
#define WOT_H_

#include "evtype.h"
#include "timer.h"
#include "chan.h"

SREY_NS_BEGIN

struct twnode : EV
{
    u_long expires;     //超时时间
    struct twnode *next;
};
struct twslot
{
    struct twnode *head;
    struct twnode *tail;
};
#define PRECISION 1000 * 1000
#define TVN_BITS (6)
#define TVR_BITS (8)
#define TVN_SIZE (1 << TVN_BITS)
#define TVR_SIZE (1 << TVR_BITS)
#define TVN_MASK (TVN_SIZE - 1)
#define TVR_MASK (TVR_SIZE - 1)
#define INDEX(N) ((m_tjiffies >> (TVR_BITS + (N) * TVN_BITS)) & TVN_MASK)

class cwot
{
public:
    cwot()
    {
        ZERO(m_tv1, sizeof(m_tv1));
        ZERO(m_tv2, sizeof(m_tv2));
        ZERO(m_tv3, sizeof(m_tv3));
        ZERO(m_tv4, sizeof(m_tv4));
        ZERO(m_tv5, sizeof(m_tv5));

        m_chan = new(std::nothrow) class cchan(ONEK);
        ASSERTAB(NULL != m_chan, ERRSTR_MEMORY);
        m_timer = new(std::nothrow) class ctimer();
        ASSERTAB(NULL != m_timer, ERRSTR_MEMORY);
    };
    ~cwot()
    {
        _free(m_tv1, TVR_SIZE);
        _free(m_tv2, TVN_SIZE);
        _free(m_tv3, TVN_SIZE);
        _free(m_tv4, TVN_SIZE);
        _free(m_tv5, TVN_SIZE);

        SAFE_DEL(m_timer);
        SAFE_DEL(m_chan);
    };
    /*
    * \brief          初始化
    */
    void init()
    {
        m_tjiffies = _msec();
    };
    /*
    * \brief          添加一超时事件
    * \param  pchan   接收超时消息
    * \param  uimsec  超时时间  毫秒
    * \param  pdata   用户数据
    * \return         true 成功
    */
    bool add(class cchan *pchan, const uint32_t &uimsec, const void *pdata)
    {
        struct twnode *pnode = new(std::nothrow) struct twnode();
        ASSERTAB(NULL != pnode, ERRSTR_MEMORY);
        pnode->data = (void*)pdata;
        pnode->chan = pchan;
        pnode->expires = _msec() + uimsec;
        pnode->next = NULL;
        pnode->evtype = EV_TIME;

        return m_chan->send((void *)pnode);
    };
    /*
    * \brief          执行时间轮
    */
    void run()
    {
        struct twnode *pnode;
        while (m_chan->canrecv())
        {
            if (m_chan->recvt(&pnode))
            {
                _insert(_getslot(pnode->expires), pnode);
            }
        }

        u_long ulnow = _msec();
        while (m_tjiffies <= ulnow)
        {
            _run();
        }
    };
    /*
    * \brief          停止
    */
    void stop()
    {
        m_chan->close();
    };

private:
    struct twslot *_getslot(u_long &ulexpires)
    {
        struct twslot *pslot;
        u_long ulidx = ulexpires - m_tjiffies;
        if ((long)ulidx < 0)
        {
            pslot = &m_tv1[(m_tjiffies & TVR_MASK)];
        }
        else if (ulidx < TVR_SIZE)
        {
            pslot = &m_tv1[(ulexpires & TVR_MASK)];
        }
        else if (ulidx < 1 << (TVR_BITS + TVN_BITS))
        {
            pslot = &m_tv2[((ulexpires >> TVR_BITS) & TVN_MASK)];
        }
        else if (ulidx < 1 << (TVR_BITS + 2 * TVN_BITS))
        {
            pslot = &m_tv3[((ulexpires >> (TVR_BITS + TVN_BITS)) & TVN_MASK)];
        }
        else if (ulidx < 1 << (TVR_BITS + 3 * TVN_BITS))
        {
            pslot = &m_tv4[((ulexpires >> (TVR_BITS + 2 * TVN_BITS)) & TVN_MASK)];
        }
        else
        {
            if (ulidx > 0xffffffffUL)
            {
                ulidx = 0xffffffffUL;
                ulexpires = ulidx + m_tjiffies;
            }
            pslot = &m_tv5[((ulexpires >> (TVR_BITS + 3 * TVN_BITS)) & TVN_MASK)];
        }

        return pslot;
    };
    u_long _msec()
    {
        return (u_long)(m_timer->nanosec() / (PRECISION));
    };
    void _run()
    {
        //调整
        u_long ulidx = m_tjiffies & TVR_MASK;
        if (!ulidx
            && (!_cascade(m_tv2, INDEX(0)))
            && (!_cascade(m_tv3, INDEX(1)))
            && (!_cascade(m_tv4, INDEX(2))))
        {
            _cascade(m_tv5, INDEX(3));
        }

        ++m_tjiffies;

        //执行
        struct twnode *pnext;
        struct twnode *pnode = m_tv1[ulidx].head;
        while (NULL != pnode)
        {
            pnext = pnode->next;
            //将超时信息发出去,发送出去后pnode有可能已经被释放，所以先取的next
            pnode->chan->send(pnode);
            pnode = pnext;
        }

        _clear(&m_tv1[ulidx]);
    };
    u_long _cascade(struct twslot *pslot, const u_long &ulindex)
    {
        struct twnode *pnext;
        struct twnode *pnode = pslot[ulindex].head;
        while (NULL != pnode)
        {
            pnext = pnode->next;
            pnode->next = NULL;
            _insert(_getslot(pnode->expires), pnode);
            pnode = pnext;
        }

        _clear(&pslot[ulindex]);

        return ulindex;
    };
    void _free(struct twslot *pslot, const size_t &uilens)
    {
        struct twnode *pnode, *pdel;
        for (size_t i = INIT_NUMBER; i < uilens; i++)
        {
            pnode = pslot[i].head;
            while (NULL != pnode)
            {
                pdel = pnode;
                pnode = pnode->next;
                SAFE_DEL(pdel);
            }
        }
    };
    void _insert(struct twslot *pslot, struct twnode *pnode)
    {
        if (NULL == pslot->head)
        {
            pslot->head = pslot->tail = pnode;
            return;
        }

        pslot->tail->next = pnode;
        pslot->tail = pnode;
    };
    void _clear(struct twslot *pslot)
    {
        pslot->head = pslot->tail = NULL;
    };

private:
    u_long m_tjiffies;
    class cchan *m_chan;
    class ctimer *m_timer;
    struct twslot m_tv1[TVR_SIZE];
    struct twslot m_tv2[TVN_SIZE];
    struct twslot m_tv3[TVN_SIZE];
    struct twslot m_tv4[TVN_SIZE];
    struct twslot m_tv5[TVN_SIZE];
};

SREY_NS_END

#endif//WOT_H_
