#ifndef BUFFER_H_
#define BUFFER_H_

#include "mutex.h"
#include "lockguard.h"

SREY_NS_BEGIN

struct buffernode
{
    struct buffernode *prior;
    struct buffernode *next;
    size_t bufferlens;
    size_t misalign;
    size_t offset;
    char *buffer;
};
struct consumecopy
{
    size_t offset;
    char *data;
};
bool _consumecopy(void *pudata, const char *pbuf, const size_t &uisize);

class cchainbuffer
{
public:
    explicit cchainbuffer(const bool block = true);
    ~cchainbuffer();
    /*
    * \brief          添加数据
    * \param pdata    数据
    * \param uisize   数据长度
    */
    void produce(const void *pdata, const size_t &uisize);
    /*
    * \brief          添加数据
    * \param pfmt     格式
    * \param ...      变参
    * \return         ERR_FAILED 失败
    * \return         成功添加的数据长度
    */
    int32_t producefmt(const char *pfmt, ...);
    /*
    * \brief            添加数据
    * \param uisize     数据长度
    * \param filler     数据填充函数,按pcave顺序填充
    *                   参数：(pudata, 填充地址数量, 填充地址, 地址对应的数据长度)
    *                   ERR_FAILED 失败 其他，返回成功填充字节数
    * \param pudata     用户参数
    * \return           ERR_FAILED 失败
    * \return           成功添加的数据长度
    */
    int32_t produce(const size_t &uisize,
        int32_t(*filler)(void *, const uint32_t &, char *pcave[2], size_t casize[2]),
        void *pudata);
    /*
    * \brief          数据总长度
    * \return         数据总长度
    */
    size_t size()
    {
        clockguard<cmutex> lockthis(&m_mutex, m_lock);
        return m_totallens;
    };
    /*
    * \brief          拷贝出数据
    * \param pdata    数据存放地址
    * \param uisize   pdata长度
    * \return         实际拷贝的数据长度
    */
    size_t copy(void *pdata, const size_t &uisize);
    /*
    * \brief          删除数据
    * \param uisize   删除多少
    */
    size_t del(const size_t &uisize);
    /*
    * \brief          拷贝并删除buffer中的数据
    * \param pdata    数据存放地址
    * \param uisize   pdata长度
    * \return         实际拷贝的数据长度
    */
    size_t remove(void *pdata, const size_t &uisize);
    /*
    * \brief          消费数据，使用后自动删除
    * \param uisize   消费长度
    * \param consumer 消费函数 参数:(用户数据, 消费的数据, 消费数据实际长度) 返回false 则终止继续执行
    * \param pudata   用户数据
    * \return         实际使用的数据长度
    */
    size_t consume(const size_t &uisize, 
        bool (*consumer)(void *, const char *, const size_t &), void *pudata);
    /*
    * \brief          获取一对象
    * \param  val     T
    * \return         true 成功
    */
    template <typename T>
    bool gett(T &val)
    {
        consumecopy stccpy;
        stccpy.offset = INIT_NUMBER;
        stccpy.data = (char*)&val;

        clockguard<cmutex> lockthis(&m_mutex, m_lock);
        if (m_totallens >= sizeof(val))
        {
            ASSERTAB(sizeof(val) == _consume(sizeof(val), _consumecopy, &stccpy), "gett error.");
            return true;
        }

        return false;
    };
    /*
    * \brief          遍历
    * \param uistart  开始位置（从0开始）
    * \param each     回调函数,返回失败，退出遍历 参数(pudata, 数据, 数据长度)
    * \param pudata   用户数据
    */
    void foreach(const size_t &uistart, bool(*each)(void *, const char *, const size_t &), void *pudata);
    /*
    * \brief          搜索，按字节比较
    * \param uistart  开始搜索的位置（从0开始）
    * \param pdata    结束位置（从0开始）  [ ]
    * \param pwhat    要搜索的数据
    * \param uiwsize  pwhat长度
    * \return         ERR_FAILED 未找到
    * \return         第一次出现的位置
    */
    int32_t search(const size_t &uistart, const size_t &uiend, const char *pwhat, const size_t &uiwsize);
    /*
    * \brief          打印出所有节点信息
    */
    void dump();

private:
    uint32_t _produce(const size_t &uisize, char *pcave[2], size_t casize[2], struct buffernode *nodes[2]); //预分配   
    void _expand(const size_t &uisize);//保证连续足够的内存    
    int32_t _producefmt(const char *pfmt, va_list args);
    size_t _consume(const size_t &uisize, bool(*consumer)(void *, const char *, const size_t &),
        void *pudata, const bool &bdel = true);
    char *_search(struct buffernode *pnode, const char *pstart, const size_t &uissize,
        const char *pwhat, const size_t &uiwsize, 
        const size_t &uitotaloff, const size_t &uiend, const bool &bend);
    bool _checkenogh(const size_t &uisize)
    {
        //为空
        if (NULL == m_tail)
        {
            _insert(_newnode(uisize));
            return true;
        }
        //剩余字节数
        if ((m_tail->bufferlens - m_tail->misalign - m_tail->offset) >= uisize)
        {
            return true;
        }
        //通过调整能否放下
        if (_should_realign(m_tail, uisize))
        {
            _align(m_tail);
            return true;
        }

        return false;
    };
    struct buffernode *_newnode(const size_t &uisize)
    {
        size_t uitotal = ROUND_UP(uisize + sizeof(struct buffernode), m_minisize);
        char *pbuf = new(std::nothrow) char[uitotal];
        ASSERTAB(NULL != pbuf, ERRSTR_MEMORY);

        struct buffernode *pnode = (struct buffernode *)pbuf;
        ZERO(pnode, sizeof(struct buffernode));
        pnode->bufferlens = uitotal - sizeof(struct buffernode);
        pnode->buffer = (char *)(pnode + 1);

        return pnode;
    };
    struct buffernode *_newnode(const size_t &uiconsult, const size_t &uisize)
    {
        size_t uialloc = uiconsult;
        if (uialloc <= ONEK * 2)
        {
            uialloc <<= 1;
        }
        if (uisize > uialloc)
        {
            uialloc = uisize;
        }

        return _newnode(uialloc);
    };
    void _deltail()
    {
        if (m_head == m_tail)
        {
            return;
        }
        if (INIT_NUMBER != m_tail->offset)
        {
            return;
        }

        struct buffernode *ptmp = m_tail->prior;
        _freenode(m_tail);
        ptmp->next = NULL;
        m_tail = ptmp;
    };
    void _freenode(struct buffernode *pnode)
    {
        char *pbuf = (char*)pnode;
        SAFE_DELARR(pbuf);
    };
    void _insert(struct buffernode *pnode)
    {
        //第一次
        if (NULL == m_tail)
        {
            m_head = m_tail = pnode;
            return;
        }

        pnode->prior = m_tail;
        m_tail->next = pnode;
        m_tail = pnode;
    };
    bool _should_realign(struct buffernode *pnode, const size_t &uisize)
    {
        return (pnode->bufferlens - pnode->offset >= uisize) &&
            (pnode->offset < pnode->bufferlens / 2) &&
            (pnode->offset <= ONEK *2);
    };
    void _align(struct buffernode *pnode)
    {
        memmove(pnode->buffer, pnode->buffer + pnode->misalign, pnode->offset);
        pnode->misalign = INIT_NUMBER;
    };

private:
    bool m_lock;
    struct buffernode *m_head;
    struct buffernode *m_tail;
    size_t m_minisize;//512 1024
    size_t m_totallens;//数据总长度
    cmutex m_mutex;
};

SREY_NS_END

#endif//BUFFER_H_
