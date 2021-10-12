#include "chainbuffer.h"
#include "utils.h"
#include "loger.h"
#include "errcode.h"

SREY_NS_BEGIN

struct buffernode
{
    struct buffernode *next;
    size_t bufferlens;
    size_t misalign;
    size_t offset;
    char *buffer;
};

#define SIZETH   2048
#define SETCAVE(pcave, size, nodes, index, pval, datelens, node)\
    pcave[index] = pval;\
    size[index] = datelens;\
    nodes[index] = node;\
    index++
#define OFFSETADDR(pnode) (pnode->buffer + pnode->misalign + pnode->offset)
#define MULOCK(block, mutex) if (block) mutex.lock()
#define MUUNLOCK(block, mutex) if (block) mutex.unlock()

cchainbuffer::cchainbuffer(const bool block)
{
    lock = block;
    head = tail = NULL;
    totallens = INIT_NUMBER;
    minisize = (sizeof(void *) < 8 ? ONEK / 2 : ONEK);
}
cchainbuffer::~cchainbuffer()
{
    struct buffernode *ptmp;
    struct buffernode *pnode = head;
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

    MULOCK(lock, mutex);
    uint32_t uicount = _produce(uisize, pcave, casize, nodes);
    for (uint32_t i = 0; i < uicount; i++)
    {
        memcpy(pcave[i], pval + uioff, casize[i]);
        uioff += casize[i];
    }
    MUUNLOCK(lock, mutex);
}
int32_t cchainbuffer::produce(const size_t &uisize,
    int32_t(*filler)(void *, const uint32_t &, char *pcave[2], size_t casize[2]),
    void *pudata)
{
    if (INIT_NUMBER == uisize)
    {
        return INIT_NUMBER;
    }

    int32_t ifill;
    char *pcave[2];
    size_t casize[2];
    struct buffernode *nodes[2];

    MULOCK(lock, mutex);
    uint32_t uicount = _produce(uisize, pcave, casize, nodes);
    ifill = filler(pudata, uicount, pcave, casize);
    //不能比分配的还多
    ASSERTAB((int32_t)uisize >= ifill, "filler error.");
    //失败
    if (ERR_FAILED == ifill)
    {
        totallens -= uisize;
        for (size_t i = 0; i < uicount; i++)
        {
            nodes[i]->offset -= casize[i];
        }
    }
    else
    {
        if ((int32_t)uisize != ifill)
        {
            //容量收缩
            size_t uicut = uisize - ifill;
            totallens -= uicut;
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
        }
    }

    if (2 == uicount
        && INIT_NUMBER == nodes[1]->offset)
    {
        _freenode(nodes[1]);
        nodes[0]->next = NULL;
        tail = nodes[0];
    }
    MUUNLOCK(lock, mutex);

    return ifill;
}
size_t cchainbuffer::size()
{
    MULOCK(lock, mutex);
    size_t uisize = totallens;
    MUUNLOCK(lock, mutex);
    
    return uisize;
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

    MULOCK(lock, mutex);
    size_t uicpy = _consume(uisize, _consumecopy, &stccpy, false);
    MUUNLOCK(lock, mutex);

    return uicpy;
}
size_t cchainbuffer::del(const size_t &uisize)
{
    MULOCK(lock, mutex);
    size_t uidel = _consume(uisize, NULL, NULL);
    MUUNLOCK(lock, mutex);

    return uidel;
}
size_t cchainbuffer::remove(void *pdata, const size_t &uisize)
{
    consumecopy stccpy;
    stccpy.offset = INIT_NUMBER;
    stccpy.data = (char*)pdata;

    MULOCK(lock, mutex);
    size_t uicpy = _consume(uisize, _consumecopy, &stccpy);
    MUUNLOCK(lock, mutex);

    return uicpy;
}
size_t cchainbuffer::consume(const size_t &uisize, bool(*consumer)(void *, const char *, const size_t &), void *pudata)
{
    MULOCK(lock, mutex);
    size_t uics = _consume(uisize, consumer, pudata);
    MUUNLOCK(lock, mutex);

    return uics;
}
void cchainbuffer::foreach(const size_t &uistart, bool(*each)(void *, const char *, const size_t &), void *pudata)
{
    clockguard<cmutex> lockthis(&mutex, lock);
    if (INIT_NUMBER == totallens
        || uistart >= totallens)
    {
        return;
    }

    bool breach = false;
    size_t uioff, uilens;
    size_t uitotaloff = INIT_NUMBER;
    struct buffernode *pnode = head;
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
    clockguard<cmutex> lockthis(&mutex, lock);
    if (INIT_NUMBER == totallens
        || uistart >= totallens
        || uiwsize > (uiend - uistart))
    {
        return ERR_FAILED;
    }

    size_t uiendoff = uiend;
    if (uiend >= totallens)
    {
        uiendoff = totallens - 1;
    }

    char *pschar = NULL;
    bool breach = false;
    bool bend = false;
    size_t uioff, uilens;
    size_t uitotaloff = INIT_NUMBER;
    struct buffernode *pnode = head;
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
            pschar = _search(pnode, pnode->buffer + pnode->misalign + uioff, uilens, pwhat, uiwsize, bend);
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
    std::string strhex;
    struct buffernode *pnode = head;
    LOG_INFO("dump buffer %X:", this);
    while (NULL != pnode)
    {
        strhex = tohex(pnode->buffer + pnode->misalign, pnode->offset);
        LOG_INFO("node %X:bufferlens %d, misalign %d, offset %d, next %X\n %s", 
            pnode, pnode->bufferlens, pnode->misalign, pnode->offset, pnode->next, strhex.c_str());

        pnode = pnode->next;
    }
}
uint32_t cchainbuffer::_produce(const size_t &uisize, char *pcave[2], size_t casize[2], struct buffernode *nodes[2])
{
    uint32_t uindex = INIT_NUMBER;
    totallens += uisize;
    struct buffernode *pnode = tail;
    //为空
    if (NULL == pnode)
    {
        pnode = _newnode(uisize);
        _insert(pnode);
    }

    //剩余字节数		
    size_t uiremain = (pnode->bufferlens - pnode->misalign - pnode->offset);
    //可以存放
    if (uiremain >= uisize)
    {
        SETCAVE(pcave, casize, nodes, uindex, OFFSETADDR(pnode), uisize, pnode);
        pnode->offset += uisize;
        return uindex;
    }
    //通过调整能否放下
    if (_should_realign(pnode, uisize))
    {
        _align(pnode);
        SETCAVE(pcave, casize, nodes, uindex, OFFSETADDR(pnode), uisize, pnode);
        pnode->offset += uisize;
        return uindex;
    }
   
    //还可以放下一些数据
    if (uiremain > INIT_NUMBER)
    {
        SETCAVE(pcave, casize, nodes, uindex, OFFSETADDR(pnode), uiremain, pnode);
        pnode->offset += uiremain;
    }

    struct buffernode *pnewnode = _newnode(pnode->bufferlens, uisize);
    //放在新节点
    uiremain = uisize - uiremain;
    SETCAVE(pcave, casize, nodes, uindex, OFFSETADDR(pnewnode), uiremain, pnewnode);
    pnewnode->offset = uiremain;

    _insert(pnewnode);

    return uindex;
}
size_t cchainbuffer::_consume(const size_t &uisize,
    bool(*consumer)(void *, const char *, const size_t &), void *pudata, const bool &bdel)
{
    if (INIT_NUMBER == uisize
        || INIT_NUMBER == totallens)
    {
        return INIT_NUMBER;
    }

    size_t uiremain = uisize;
    if (uisize > totallens)
    {
        uiremain = totallens;
    }

    size_t uiconsumed = INIT_NUMBER;
    struct buffernode *ptmp;
    struct buffernode *pnode = head;
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
                    head = pnode;
                }
                return uiconsumed;
            }
        }

        uiconsumed += pnode->offset;
        if (pnode == tail)
        {
            if (bdel)
            {
                totallens -= pnode->offset;
                pnode->misalign = 0;
                pnode->offset = 0;                
            }
            break;
        }

        ptmp = pnode->next;
        if (bdel)
        {
            totallens -= pnode->offset;
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
                    head = pnode;
                }
                return uiconsumed;
            }
        }

        uiconsumed += uiremain;
        if (bdel)
        {
            totallens -= uiremain;
            pnode->misalign += uiremain;
            pnode->offset -= uiremain;
        }
    }

    if (bdel)
    {
        head = pnode;
    }

    return uiconsumed;
}
char *cchainbuffer::_search(struct buffernode *pnode, const char *pstart, const size_t &uissize,
    const char *pwhat, const size_t &uiwsize, const bool &bend)
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
            return _search(pnode, pspos + 1, uiremain - 1, pwhat, uiwsize, bend);
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

        //处理还有一部分在下一个节点的情况
        if (ERR_OK == memcmp(pspos, pwhat, uiremain))
        {
            if (ERR_OK == memcmp(pnext->buffer + pnext->misalign, pwhat + uiremain, uiwsize - uiremain))
            {
                return pspos;
            }
            else
            {
                return _search(pnode, pspos + 1, uiremain - 1, pwhat, uiwsize, bend);
            }
        }
        else
        {
            return _search(pnode, pspos + 1, uiremain - 1, pwhat, uiwsize, bend);
        }
    }

    return NULL;
}
struct buffernode *cchainbuffer::_newnode(const size_t &uisize)
{
    size_t uitotal = ROUND_UP(uisize + sizeof(struct buffernode), minisize);
    char *pbuf = new(std::nothrow) char[uitotal];
    ASSERTAB(NULL != pbuf, ERRSTR_MEMORY);

    struct buffernode *pnode = (struct buffernode *)pbuf;
    ZERO(pnode, sizeof(struct buffernode));
    pnode->bufferlens = uitotal - sizeof(struct buffernode);
    pnode->buffer = (char *)(pnode + 1);

    return pnode;
}
struct buffernode *cchainbuffer::_newnode(const size_t &uiconsult, const size_t &uisize)
{
    size_t uialloc = uiconsult;
    if (uialloc <= SIZETH)
    {
        uialloc <<= 1;
    }
    if (uisize > uialloc)
    {
        uialloc = uisize;
    }

    return _newnode(uialloc);
}
void cchainbuffer::_freenode(struct buffernode *pnode)
{
    char *pbuf = (char*)pnode;
    SAFE_DELARR(pbuf);
}
void cchainbuffer::_insert(struct buffernode *pnode)
{
    //第一次
    if (NULL == tail)
    {
        head = tail = pnode;
        return;
    }

    tail->next = pnode;
    tail = pnode;
}
bool cchainbuffer::_should_realign(struct buffernode *pnode, const size_t &uisize)
{
    return (pnode->bufferlens - pnode->offset >= uisize) &&
        (pnode->offset < pnode->bufferlens / 2) &&
        (pnode->offset <= SIZETH);
}
void cchainbuffer::_align(struct buffernode *pnode)
{
    memmove(pnode->buffer, pnode->buffer + pnode->misalign, pnode->offset);
    pnode->misalign = INIT_NUMBER;
}

SREY_NS_END
