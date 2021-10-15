#include "chainbuffer.h"
#include "utils.h"
#include "loger.h"

SREY_NS_BEGIN

#define SETCAVE(pcave, size, nodes, index, pval, datelens, node)\
    pcave[index] = pval;\
    size[index] = datelens;\
    nodes[index] = node;\
    index++
#define SETHEAD(pnode) m_head = pnode; m_head->prior = NULL
#define OFFSETADDR(pnode) (pnode->buffer + pnode->misalign + pnode->offset)

cchainbuffer::cchainbuffer(const bool block) : 
    m_lock(block), m_head(NULL), m_tail(NULL), 
    m_minisize(sizeof(void *) < 8 ? ONEK / 2 : ONEK),
    m_totallens(INIT_NUMBER)
{ }
cchainbuffer::~cchainbuffer()
{
    struct buffernode *ptmp;
    struct buffernode *pnode = m_head;
    while (NULL != pnode)
    {
        ptmp = pnode;
        pnode = pnode->next;
        _freenode(ptmp);
    }
}
void cchainbuffer::produce(const void *pdata, const size_t &uisize)
{
    char *pcave[2];
    size_t casize[2];
    struct buffernode *nodes[2];
    char *pval = (char *)pdata;
    size_t uioff = INIT_NUMBER;
    clockguard<cmutex> lockthis(&m_mutex, m_lock);

    uint32_t uicount = _produce(uisize, pcave, casize, nodes);
    for (uint32_t i = 0; i < uicount; i++)
    {
        memcpy(pcave[i], pval + uioff, casize[i]);
        uioff += casize[i];
    }
}
int32_t cchainbuffer::producefmt(const char *pfmt, ...)
{
    va_list va;
    clockguard<cmutex> lockthis(&m_mutex, m_lock);

    va_start(va, pfmt);
    int32_t icount = _producefmt(pfmt, va);
    va_end(va);

    return icount;
}
int32_t cchainbuffer::produce(const size_t &uisize,
    int32_t(*filler)(void *, const uint32_t &, char *pcave[2], size_t casize[2]),
    void *pudata)
{
    if (INIT_NUMBER == uisize)
    {
        return INIT_NUMBER;
    }

    char *pcave[2];
    size_t casize[2];
    struct buffernode *nodes[2];
    clockguard<cmutex> lockthis(&m_mutex, m_lock);

    uint32_t uicount = _produce(uisize, pcave, casize, nodes);
    int32_t ifill = filler(pudata, uicount, pcave, casize);
    ASSERTAB((int32_t)uisize >= ifill, "filler error.");
    //失败
    if (ERR_FAILED == ifill)
    {
        m_totallens -= uisize;
        for (size_t i = 0; i < uicount; i++)
        {
            nodes[i]->offset -= casize[i];
        }
        _deltail();

        return ERR_FAILED;
    }
    if ((int32_t)uisize != ifill)
    {
        //容量收缩
        size_t uicut = uisize - ifill;
        m_totallens -= uicut;
        for (size_t i = uicount - 1; i >= 0; i--)
        {
            if (casize[i] >= uicut)
            {
                nodes[i]->offset -= uicut;
                break;
            }
            else
            {
                nodes[i]->offset -= casize[i];
                uicut -= casize[i];
            }
        }
        _deltail();
    }

    return ifill;
}
bool _consumecopy(void *pudata, const char *pbuf, const size_t &uisize)
{
    consumecopy *pccpy = (consumecopy*)pudata;

    memcpy(pccpy->data + pccpy->offset, pbuf, uisize);
    pccpy->offset += uisize;

    return true;
}
size_t cchainbuffer::copy(void *pdata, const size_t &uisize)
{
    consumecopy stccpy;
    stccpy.offset = INIT_NUMBER;
    stccpy.data = (char*)pdata;
    clockguard<cmutex> lockthis(&m_mutex, m_lock);

    return _consume(uisize, _consumecopy, &stccpy, false);
}
size_t cchainbuffer::del(const size_t &uisize)
{
    clockguard<cmutex> lockthis(&m_mutex, m_lock);
    return _consume(uisize, NULL, NULL);
}
size_t cchainbuffer::remove(void *pdata, const size_t &uisize)
{
    consumecopy stccpy;
    stccpy.offset = INIT_NUMBER;
    stccpy.data = (char*)pdata;
    clockguard<cmutex> lockthis(&m_mutex, m_lock);

    return _consume(uisize, _consumecopy, &stccpy);
}
size_t cchainbuffer::consume(const size_t &uisize, bool(*consumer)(void *, const char *, const size_t &), void *pudata)
{
    clockguard<cmutex> lockthis(&m_mutex, m_lock);
    return _consume(uisize, consumer, pudata);
}
void cchainbuffer::foreach(const size_t &uistart, bool(*each)(void *, const char *, const size_t &), void *pudata)
{
    clockguard<cmutex> lockthis(&m_mutex, m_lock);
    if (INIT_NUMBER == m_totallens
        || uistart >= m_totallens)
    {
        return;
    }

    bool breach = false;
    size_t uioff, uilens;
    size_t uitotaloff = INIT_NUMBER;
    struct buffernode *pnode = m_head;
    while (NULL != pnode)
    {
        uioff = INIT_NUMBER;
        uilens = pnode->offset;
        if (!breach)
        {
            uitotaloff += pnode->offset;
            //到达开始位置的节点
            if (uitotaloff > uistart)
            {
                breach = true;
                uioff = pnode->offset - (uitotaloff - uistart);
                uilens = pnode->offset - uioff;
            }
        }
        if(breach)
        {
            if (!each(pudata, pnode->buffer + pnode->misalign + uioff, uilens))
            {
                break;
            }
        }

        pnode = pnode->next;
    }
}
int32_t cchainbuffer::search(const size_t &uistart, const size_t &uiend, const char *pwhat, const size_t &uiwsize)
{
    clockguard<cmutex> lockthis(&m_mutex, m_lock);
    if (INIT_NUMBER == m_totallens
        || uistart >= m_totallens
        || uiwsize > (uiend - uistart) + 1)
    {
        return ERR_FAILED;
    }

    size_t uiendoff = uiend;
    if (uiend >= m_totallens)
    {
        uiendoff = m_totallens - 1;
    }

    char *pschar = NULL;
    bool breach = false;
    bool bend = false;
    size_t uioff, uilens;
    size_t uitotaloff = INIT_NUMBER;
    struct buffernode *pnode = m_head;
    while (NULL != pnode)
    {
        uioff = INIT_NUMBER;
        uilens = pnode->offset;
        uitotaloff += pnode->offset;
        if (!breach)
        {
            //到达开始位置的节点
            if (uitotaloff > uistart)
            {
                breach = true;
                uioff = pnode->offset - (uitotaloff - uistart);
                uilens = pnode->offset - uioff;
            }
        }
        if (breach)
        {
            //到达结束位置的节点
            if (uitotaloff > uiendoff)
            {
                bend = true;
                uilens = pnode->offset - (uitotaloff - uiendoff) - uioff + 1;
            }
            pschar = _search(pnode, pnode->buffer + pnode->misalign + uioff, uilens, 
                pwhat, uiwsize, uitotaloff, uiendoff, bend);
            if (NULL != pschar)
            {
                return (int32_t)(uitotaloff - pnode->offset + pschar - (pnode->buffer + pnode->misalign));
            }
            if (bend)
            {
                break;
            }
        }

        pnode = pnode->next;
    }

    return ERR_FAILED;
}
void cchainbuffer::dump()
{
    clockguard<cmutex> lockthis(&m_mutex, m_lock);

    struct buffernode *pnode = m_head;
    LOG_INFO("dump buffer %X:", this);
    while (NULL != pnode)
    {
        LOG_INFO("node %X:bufferlens %d, misalign %d, offset %d, prior %X, next %X\n %s", 
            pnode, pnode->bufferlens, pnode->misalign, pnode->offset, pnode->prior, pnode->next,
            tohex(pnode->buffer + pnode->misalign, pnode->offset).c_str());

        pnode = pnode->next;
    }
}
uint32_t cchainbuffer::_produce(const size_t &uisize, char *pcave[2], size_t casize[2], struct buffernode *nodes[2])
{
    uint32_t uindex = INIT_NUMBER;
    m_totallens += uisize;
    if (_checkenogh(uisize))
    {
        SETCAVE(pcave, casize, nodes, uindex, OFFSETADDR(m_tail), uisize, m_tail);
        m_tail->offset += uisize;
        return uindex;
    }

    //剩余字节数		
    size_t uiremain = (m_tail->bufferlens - m_tail->misalign - m_tail->offset);
    //还可以放下一些数据
    if (uiremain > INIT_NUMBER)
    {
        SETCAVE(pcave, casize, nodes, uindex, OFFSETADDR(m_tail), uiremain, m_tail);
        m_tail->offset += uiremain;
    }

    struct buffernode *pnew = _newnode(m_tail->bufferlens, uisize);
    //放在新节点
    uiremain = uisize - uiremain;
    SETCAVE(pcave, casize, nodes, uindex, OFFSETADDR(pnew), uiremain, pnew);
    pnew->offset = uiremain;

    _insert(pnew);

    return uindex;
}
void cchainbuffer::_expand(const size_t &uisize)
{
    if (_checkenogh(uisize))
    {
        return;
    }

    struct buffernode *pnew = _newnode(uisize);
    //无数据
    if (INIT_NUMBER == m_tail->offset)
    {
        //第一个节点
        if (m_head == m_tail)
        {
            _freenode(m_tail);
            m_head = m_tail = pnew;
        }
        else
        {
            _deltail();
            _insert(pnew);
        }
    }
    else
    {
        _insert(pnew);
    }
}
int32_t cchainbuffer::_producefmt(const char *pfmt, va_list args)
{
    char *pbuf;
    size_t uiremain;
    int32_t icount;

    _expand(64);
    for (int32_t i = INIT_NUMBER; i < 2; i++)
    {
        pbuf = m_tail->buffer + m_tail->misalign + m_tail->offset;
        uiremain = m_tail->bufferlens - m_tail->misalign - m_tail->offset;

        icount = vsnprintf(pbuf, uiremain, pfmt, args);
        if (icount < INIT_NUMBER)
        {
            _deltail();
            return ERR_FAILED;
        }
        if (icount < (int32_t)uiremain)
        {
            m_totallens += icount;
            m_tail->offset += icount;
            return icount;
        }
        if (INIT_NUMBER == i)
        {
            _expand(icount + 1);
        }        
    }

    _deltail();

    return ERR_FAILED;
}
size_t cchainbuffer::_consume(const size_t &uisize,
    bool(*consumer)(void *, const char *, const size_t &), void *pudata, const bool &bdel)
{
    if (INIT_NUMBER == uisize
        || INIT_NUMBER == m_totallens)
    {
        return INIT_NUMBER;
    }

    size_t uiremain = uisize;
    if (uisize > m_totallens)
    {
        uiremain = m_totallens;
    }

    size_t uiconsumed = INIT_NUMBER;
    struct buffernode *ptmp;
    struct buffernode *pnode = m_head;
    while (uiremain > INIT_NUMBER
        && uiremain >= pnode->offset)
    {
        uiremain -= pnode->offset;
        if (NULL != consumer)
        {
            if (!consumer(pudata, pnode->buffer + pnode->misalign, pnode->offset))
            {
                if (bdel)
                {
                    SETHEAD(pnode);
                }
                return uiconsumed;
            }
        }

        uiconsumed += pnode->offset;
        if (pnode == m_tail)
        {
            if (bdel)
            {
                m_totallens -= pnode->offset;
                pnode->misalign = 0;
                pnode->offset = 0;                
            }
            break;
        }

        ptmp = pnode->next;
        if (bdel)
        {
            m_totallens -= pnode->offset;
            _freenode(pnode);
        }
        pnode = ptmp;
    }
    if (INIT_NUMBER != uiremain)
    {
        if (NULL != consumer)
        {
            if (!consumer(pudata, pnode->buffer + pnode->misalign, uiremain))
            {
                if (bdel)
                {
                    SETHEAD(pnode);
                }
                return uiconsumed;
            }
        }

        uiconsumed += uiremain;
        if (bdel)
        {
            m_totallens -= uiremain;
            pnode->misalign += uiremain;
            pnode->offset -= uiremain;
        }
    }

    if (bdel)
    {
        SETHEAD(pnode);
    }

    return uiconsumed;
}
char *cchainbuffer::_search(struct buffernode *pnode, const char *pstart, const size_t &uissize,
    const char *pwhat, const size_t &uiwsize,
    const size_t &uitotaloff, const size_t &uiend, const bool &bend)
{
    if (INIT_NUMBER == uissize)
    {
        return NULL;
    }
    char *pspos = (char *)memchr(pstart, pwhat[0], uissize);
    if (NULL == pspos)
    {
        return NULL;
    }

    size_t uiremain = uissize - (pspos - pstart);
    //剩余长度足够
    if (uiremain >= uiwsize)
    {
        if (ERR_OK == memcmp(pspos, pwhat, uiwsize))
        {
            return pspos;
        }
        else
        {
            return _search(pnode, pspos + 1, uiremain - 1, pwhat, uiwsize, uitotaloff, uiend, bend);
        }
    }
    else
    {
        if (bend)
        {
            return NULL;
        }
        struct buffernode *pnext = pnode->next;
        if (NULL == pnext)
        {
            return NULL;
        }
        size_t uinext = uiwsize - uiremain;
        if (uinext > pnext->offset)
        {
            return NULL;
        }
        if (uitotaloff + uinext > uiend + 1)
        {
            return NULL;
        }

        //处理还有一部分在下一个节点的情况
        if (ERR_OK == memcmp(pspos, pwhat, uiremain))
        {
            if (ERR_OK == memcmp(pnext->buffer + pnext->misalign, pwhat + uiremain, uinext))
            {
                return pspos;
            }
            else
            {
                return _search(pnode, pspos + 1, uiremain - 1, pwhat, uiwsize, uitotaloff, uiend, bend);
            }
        }
        else
        {
            return _search(pnode, pspos + 1, uiremain - 1, pwhat, uiwsize, uitotaloff, uiend, bend);
        }
    }

    return NULL;
}

SREY_NS_END
