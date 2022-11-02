#include "buffer.h"

struct bufnode_ctx
{
    int32_t used;
    struct bufnode_ctx *next;
    char *buffer;
    size_t buffer_len;
    size_t misalign;
    size_t off;
};
#define MAX_COPY_IN_EXPAND 4096
#define MAX_REALIGN_IN_EXPAND 2048
#define FIRST_FORMAT_IN_EXPAND 64
#define NODE_SPACE_PTR(ch) ((ch)->buffer + (ch)->misalign + (ch)->off)
#define NODE_SPACE_LEN(ch) ((ch)->buffer_len - ((ch)->misalign + (ch)->off))
#define RECOED_IOV(ch, lens) \
    piov[index].IOV_PTR_FIELD = NODE_SPACE_PTR(ch);\
    piov[index].IOV_LEN_FIELD = (IOV_LEN_TYPE)lens;\
    ch->used = 1;\
    index++

//新建一节点
static struct bufnode_ctx *_node_new(const size_t uisize)
{
    size_t uitotal = ROUND_UP(uisize + sizeof(struct bufnode_ctx),
        sizeof(void *) < 8 ? 512 : ONEK);
    char *pbuf;
    MALLOC(pbuf, uitotal);
    struct bufnode_ctx *pnode = (struct bufnode_ctx *)pbuf;
    ZERO(pnode, sizeof(struct bufnode_ctx));
    pnode->buffer_len = uitotal - sizeof(struct bufnode_ctx);
    pnode->buffer = (char *)(pnode + 1);

    return pnode;
}
//通过调整是否足够
static int _should_realign(struct bufnode_ctx *pnode, const size_t uilens)
{
    return pnode->buffer_len - pnode->off >= uilens &&
        (pnode->off < pnode->buffer_len / 2) &&
        (pnode->off <= MAX_REALIGN_IN_EXPAND);
}
//调整
static void _align(struct bufnode_ctx *pnode)
{
    memmove(pnode->buffer, pnode->buffer + pnode->misalign, pnode->off);
    pnode->misalign = 0;
}
//释放pnode及后面节点
static void _free_all_node(struct bufnode_ctx *pnode)
{
    struct bufnode_ctx *pnext;
    for (; NULL != pnode; pnode = pnext) 
    {
        pnext = pnode->next;
        FREE(pnode);
    }
}
//pnode 及后面节点是否为空
static int _node_all_empty(struct bufnode_ctx *pnode)
{
    for (; NULL != pnode; pnode = pnode->next)
    {
        if (0 != pnode->off)
        {
            return 0;
        }
    }
    return 1;
}
//释放空节点
static struct bufnode_ctx **_free_trailing_empty_node(struct buffer_ctx *pctx)
{
    struct bufnode_ctx **pnode = pctx->tail_with_data;
    while (NULL != (*pnode) 
        && (*pnode)->off != 0)
    {
        pnode = &(*pnode)->next;
    }
    if (NULL != *pnode) 
    {
        ASSERTAB(_node_all_empty(*pnode), "node must empty.");
        _free_all_node(*pnode);
        *pnode = NULL;
    }

    return pnode;
}
//插入,会清理空节点
static void _node_insert(struct buffer_ctx *pctx, struct bufnode_ctx *pnode)
{
     if (NULL == *pctx->tail_with_data)
     {
        ASSERTAB(pctx->tail_with_data == &pctx->head, "tail_with_data not equ head.");
        ASSERTAB(pctx->head == NULL, "head not NULL.");
        pctx->head = pctx->tail = pnode;
    }
    else 
    {
        struct bufnode_ctx **plast = _free_trailing_empty_node(pctx);
        *plast = pnode;
        if (0 != pnode->off)
        {
            pctx->tail_with_data = plast;
        }            
        pctx->tail = pnode;
    }
    pctx->total_len += pnode->off;
}
//新建节点并插入
static struct bufnode_ctx *_node_insert_new(struct buffer_ctx *pctx, const size_t uilens)
{
    struct bufnode_ctx *pnode = _node_new(uilens);
    _node_insert(pctx, pnode);
    return pnode;
}
//扩展空间，保证连续内存
struct bufnode_ctx *buffer_expand_single(struct buffer_ctx *pctx, const size_t uilens)
{
    struct bufnode_ctx *pnode, **plast;
    plast = pctx->tail_with_data;

    //最后一个有数据的节点满的
    if (NULL != *plast 
        && 0 == NODE_SPACE_LEN(*plast))
    {
        plast = &(*plast)->next;
    }
    pnode = *plast;
    if (NULL == pnode)
    {
        return _node_insert_new(pctx, uilens);
    }
    if (NODE_SPACE_LEN(pnode) >= uilens) 
    {
        return pnode;
    }
    if (0 == pnode->off) 
    {
        //插入新的，并删除pnode
        return _node_insert_new(pctx, uilens);
    }
    //调整
    if (_should_realign(pnode, uilens)) 
    {
        _align(pnode);
        return pnode;
    }
    //空闲空间小于总空间的1/8 或者 已有的数据量大于MAX_COPY_IN_EXPAND(4096)
    if (NODE_SPACE_LEN(pnode) < pnode->buffer_len / 8 
        || pnode->off > MAX_COPY_IN_EXPAND) 
    {
        if (NULL != pnode->next 
            && NODE_SPACE_LEN(pnode->next) >= uilens) 
        {
            return pnode->next;
        }
        else 
        {
            return _node_insert_new(pctx, uilens);
        }
    }
    
    //数据迁移
    struct bufnode_ctx *ptmp = _node_new(pnode->off + uilens);
    ptmp->off = pnode->off;
    memcpy(ptmp->buffer, pnode->buffer + pnode->misalign, pnode->off);
    ASSERTAB(*plast == pnode, "tail_with_data not equ pnode.");
    *plast = ptmp;
    if (pctx->tail == pnode)
    {
        pctx->tail = ptmp;
    }

    ptmp->next = pnode->next;
    FREE(pnode);
    return ptmp;
}
uint32_t buffer_expand_iov(struct buffer_ctx *pctx, const size_t uilens, 
    IOV_TYPE *piov, const uint32_t uicnt)
{
    struct bufnode_ctx *ptmp, *pnext, *pnode = pctx->tail;
    size_t uiavail, uiremain, uiused, uispace;
    uint32_t index = 0;
    
    ASSERTAB(uicnt >= 2, "param error.");
    if (NULL == pnode)
    {
        pnode = _node_new(uilens);
        _node_insert(pctx, pnode);
        RECOED_IOV(pnode, uilens);
        return index;
    }

    uiused = 0; //使用了多少个节点
    uiavail = 0;//可用空间
    for (pnode = *pctx->tail_with_data; NULL != pnode; pnode = pnode->next) 
    {
        if (0 != pnode->off) 
        {
            uispace = (size_t)NODE_SPACE_LEN(pnode);
            ASSERTAB(pnode == *pctx->tail_with_data, "ail_with_data not equ pnode.");
            if (0 != uispace) 
            {
                uiavail += uispace;
                ++uiused;
                RECOED_IOV(pnode, uispace);
            }
        }
        else 
        {
            pnode->misalign = 0;
            uiavail += pnode->buffer_len;
            ++uiused;
            RECOED_IOV(pnode, pnode->buffer_len);
        }
        if (uiavail >= uilens) 
        {
            return index;
        }
        if (uiused == uicnt)
        {
            break;
        }
    }
    //没有达到最大节点数，空间还不够
    if (uiused < uicnt) 
    {
        uiremain = uilens - uiavail;
        ASSERTAB(NULL == pnode, "pnode not equ NULL.");
        ptmp = _node_new(uiremain);
        pctx->tail->next = ptmp;
        pctx->tail = ptmp;
        RECOED_IOV(ptmp, uiremain);
        return index;
    }
    
    //所有节点都不能装下
    index = 0;
    int32_t idelall = 0;
    pnode = *pctx->tail_with_data;
    if (0 == pnode->off)//无数据
    {
        ASSERTAB(pnode == pctx->head, "head not equ pnode.");
        idelall = 1;
        uiavail = 0;
    }
    else
    {
        uiavail = (size_t)NODE_SPACE_LEN(pnode);
        if (0 != uiavail)
        {
            RECOED_IOV(pnode, uiavail);
        }        
        pnode = pnode->next;        
    }
    //释放
    for (; NULL != pnode; pnode = pnext)
    {
        pnext = pnode->next;
        ASSERTAB(0 == pnode->off, "node not empty.");
        FREE(pnode);
    }
    ASSERTAB(uilens >= uiavail, "logic error.");
    uiremain = uilens - uiavail;
    ptmp = _node_new(uiremain);
    RECOED_IOV(ptmp, uiremain);
    if (idelall)
    {
        pctx->head = pctx->tail = ptmp;
        pctx->tail_with_data = &pctx->head;
    }
    else
    {
        (*pctx->tail_with_data)->next = ptmp;
        pctx->tail = ptmp;
    }

    return index;
}
static void _last_with_data(struct buffer_ctx *pctx)
{
    struct bufnode_ctx **pnode = pctx->tail_with_data;
    if (NULL == *pnode)
    {
        return;
    }
    while (NULL != (*pnode)->next)
    {
        pnode = &(*pnode)->next;
        if (0 != (*pnode)->off)
        {
            pctx->tail_with_data = pnode;
        }
    }
}
size_t buffer_commit_iov(struct buffer_ctx *pctx, size_t uilens, IOV_TYPE *piov, const uint32_t uicnt)
{
    if (0 == uicnt)
    {
        return 0;
    }

    //只有一个
    if (1 == uicnt
        && NULL != pctx->tail 
        && piov[0].IOV_PTR_FIELD == (void *)NODE_SPACE_PTR(pctx->tail))
    {
        ASSERTAB(uilens <= (size_t)NODE_SPACE_LEN(pctx->tail), "logic error.");
        pctx->tail->used = 0;
        pctx->tail->off += uilens;
        pctx->total_len += uilens;
        if (0 != uilens)
        {
            _last_with_data(pctx);
        }

        return uilens;
    }

    uint32_t i;
    struct bufnode_ctx *pnode, **pfirst, **pfill;
    pfirst = pctx->tail_with_data;
    if (NULL == *pfirst)
    {
        return 0;
    }
    if (0 == NODE_SPACE_LEN(*pfirst))
    {
        pfirst = &(*pfirst)->next;
    }
    
    //检查
    pnode = *pfirst;
    for (i = 0; i < uicnt; ++i)
    {
        if (NULL == pnode)
        {
            return 0;
        }
        if (piov[i].IOV_PTR_FIELD != (void *)NODE_SPACE_PTR(pnode))
        {
            return 0;
        }
        pnode = pnode->next;
    }
    //填充
    size_t uiadded = 0;
    pfill = pfirst;    
    for (i = 0; i < uicnt; ++i)
    {
        (*pfill)->used = 0;
        if (uilens > 0)
        {
            if (uilens >= piov[i].IOV_LEN_FIELD)
            {
                (*pfill)->off += piov[i].IOV_LEN_FIELD;
                uiadded += piov[i].IOV_LEN_FIELD;
                uilens -= piov[i].IOV_LEN_FIELD;
            }
            else
            {
                (*pfill)->off += uilens;
                uiadded += uilens;
                uilens = 0;
            }
        }
        if ((*pfill)->off > 0)
        {
            pctx->tail_with_data = pfill;
        }
        pfill = &(*pfill)->next;
    }
    ASSERTAB(0 == uilens, "logic error.");
    pctx->total_len += uiadded;

    return uiadded;
}
void buffer_lock(struct buffer_ctx *pctx)
{
    mutex_lock(&pctx->mutex);
}
void buffer_unlock(struct buffer_ctx *pctx)
{
    mutex_unlock(&pctx->mutex);
}
void buffer_init(struct buffer_ctx *pctx)
{
    ZERO(pctx, sizeof(struct buffer_ctx));
    pctx->tail_with_data = &pctx->head;
    mutex_init(&pctx->mutex);
}
void buffer_free(struct buffer_ctx *pctx)
{
    _free_all_node(pctx->head);
    mutex_free(&pctx->mutex);
}
int32_t buffer_append(struct buffer_ctx *pctx, void *pdata, const size_t uilen)
{
    char *ptmp = (char*)pdata;
    if (0 != pctx->freeze_write)
    {
        PRINT("%s", "tail locked.");
        return ERR_FAILED;
    }

    IOV_TYPE piov[2];
    size_t uiremain = uilen;
    size_t i, uioff = 0;
    uint32_t uinum = buffer_expand_iov(pctx, uilen, piov, 2);
    for (i = 0; i < uinum && uiremain > 0; i++)
    {
        if ((IOV_LEN_TYPE)uiremain >= piov[i].IOV_LEN_FIELD)
        {
            memcpy(piov[i].IOV_PTR_FIELD, ptmp + uioff, piov[i].IOV_LEN_FIELD);
            uioff += piov[i].IOV_LEN_FIELD;
            uiremain -= piov[i].IOV_LEN_FIELD;
        }
        else
        {
            memcpy(piov[i].IOV_PTR_FIELD, ptmp + uioff, uiremain);
            piov[i].IOV_LEN_FIELD = (IOV_LEN_TYPE)uiremain;
            uiremain = 0;
        }
    }
    ASSERTAB(uilen == buffer_commit_iov(pctx, uilen, piov, uinum), "commit lens not equ buffer lens.");

    return ERR_OK;
}
int32_t buffer_appendv(struct buffer_ctx *pctx, const char *pfmt, ...)
{
    if (0 != pctx->freeze_write)
    {
        PRINT("%s", "tail locked.");
        return ERR_FAILED;
    }

    va_list va;
    int32_t irtn, isize;

    struct bufnode_ctx *pnode = buffer_expand_single(pctx, FIRST_FORMAT_IN_EXPAND);
    pnode->used = 1;
    va_start(va, pfmt);
    while (1)
    {
        isize = (int32_t)NODE_SPACE_LEN(pnode);
        irtn = vsnprintf(NODE_SPACE_PTR(pnode), (size_t)isize, pfmt, va);
        if ((irtn > -1)
            && (irtn < isize))
        {
            pnode->used = 0;
            pnode->off += irtn;
            pctx->total_len += irtn;
            break;
        }

        pnode->used = 0;
        pnode = buffer_expand_single(pctx, irtn + 1);
        pnode->used = 1;
    }
    va_end(va);
    return ERR_OK;
}
int32_t buffer_copyout(struct buffer_ctx *pctx, void *pout, size_t uilens)
{
    if (0 != pctx->freeze_read)
    {
        PRINT("%s", "head locked.");
        return ERR_FAILED;
    }
    struct bufnode_ctx *pnode = pctx->head;
    char *pdata = pout;
    if (uilens > pctx->total_len)
    {
        uilens = pctx->total_len;
    }
    if (0 == uilens)
    {
        return 0;
    }

    size_t uinread = uilens;
    while (0 != uilens
        && uilens >= pnode->off)
    {
        memcpy(pdata, pnode->buffer + pnode->misalign, pnode->off);
        pdata += pnode->off;
        uilens -= pnode->off;
        pnode = pnode->next;
    }
    if (0 != uilens)
    {
        memcpy(pdata, pnode->buffer + pnode->misalign, uilens);
    }

    return (int32_t)uinread;
}
int32_t buffer_drain(struct buffer_ctx *pctx, size_t uilen)
{
    if (0 != pctx->freeze_read)
    {
        PRINT("%s", "head locked.");
        return ERR_FAILED;
    }
    struct bufnode_ctx *pnode, *pnext;
    size_t uiremain, uioldlen;
    uioldlen = pctx->total_len;
    if (0 == uioldlen)
    {
        return 0;
    }
    if (uilen > uioldlen)
    {
        uilen = uioldlen;
    }
    pctx->total_len -= uilen;
    uiremain = uilen;
    for (pnode = pctx->head; NULL != pnode && uiremain >= pnode->off; pnode = pnext)
    {
        pnext = pnode->next;
        uiremain -= pnode->off;
        if (pnode == *pctx->tail_with_data)
        {
            pctx->tail_with_data = &pctx->head;
        }
        if (&pnode->next == pctx->tail_with_data)
        {
            pctx->tail_with_data = &pctx->head;
        }
        if (0 == pnode->used)
        {
            FREE(pnode);
        }
        else
        {
            ASSERTAB(0 == uiremain, "logic error.");
            pnode->misalign += pnode->off;
            pnode->off = 0;
            break;
        }
    }

    pctx->head = pnode;
    if (NULL != pnode)
    {
        ASSERTAB(uiremain <= pnode->off, "logic error.");
        pnode->misalign += uiremain;
        pnode->off -= uiremain;
    }
    else
    {
        pctx->head = pctx->tail = NULL;
        pctx->tail_with_data = &(pctx)->head;
    }

    return (int32_t)uilen;
}
int32_t buffer_remove(struct buffer_ctx *pctx, void *pout, size_t uilen)
{
    int32_t irtn = buffer_copyout(pctx, pout, uilen);
    if (irtn > 0)
    {
        ASSERTAB(irtn == buffer_drain(pctx, irtn), "drain lens not equ copy lens.");
    }
    return irtn;
}
static int32_t _search_memcmp(struct bufnode_ctx *pnode, size_t uioff, char *pwhat, size_t uiwlens)
{
    size_t uincomp;
    while (uiwlens > 0
        && NULL != pnode)
    {
        uincomp = uiwlens + uioff > pnode->off ? pnode->off - uioff : uiwlens;
        if (0 != memcmp(pnode->buffer + pnode->misalign + uioff, pwhat, uincomp))
        {
            return ERR_FAILED;
        }

        pwhat += uincomp;
        uiwlens -= uincomp;
        uioff = 0;
        pnode = pnode->next;
    }

    return ERR_OK;
}
static struct bufnode_ctx *_search_start(struct bufnode_ctx *pnode, size_t uistart, size_t *ptotaloff)
{
    while (NULL != pnode
        && 0 != pnode->off)
    {
        *ptotaloff += pnode->off;
        //到达开始位置的节点
        if (*ptotaloff > uistart)
        {
            return pnode;
        }

        pnode = pnode->next;
    }

    return NULL;
}
static int32_t buffer_search(struct buffer_ctx *pctx, const size_t uistart, char *pwhat, size_t uiwlens)
{
    if (0 != pctx->freeze_read)
    {
        PRINT("%s", "head locked.");
        return ERR_FAILED;
    }
    if (uistart >= pctx->total_len
        || uiwlens > pctx->total_len)
    {
        return ERR_FAILED;
    }
    
    //查找开始位置所在节点
    size_t uitotaloff = 0;
    struct bufnode_ctx *pnode = _search_start(pctx->head, uistart, &uitotaloff);
    ASSERTAB(NULL != pnode && 0 != pnode->off, "can't search start node.");

    char *pschar, *pstart;
    size_t uioff = pnode->off - (uitotaloff - uistart);
    while (NULL != pnode
        && 0 != pnode->off)
    {
        pstart = pnode->buffer + pnode->misalign + uioff;
        pschar = (char *)memchr(pstart, pwhat[0], pnode->off - uioff);
        if (NULL != pschar)
        {
            uioff += (pschar - pstart);
            if (ERR_OK == _search_memcmp(pnode, uioff, (char *)pwhat, uiwlens))
            {
                return (int32_t)(uitotaloff - pnode->off + uioff);
            }
            
            uioff++;
            if (pnode->off == uioff)
            {
                uioff = 0;
                pnode = pnode->next;
                if (NULL != pnode)
                {
                    uitotaloff += pnode->off;
                }
            }
        }
        else
        {
            uioff = 0;
            pnode = pnode->next;
            if (NULL != pnode)
            {
                uitotaloff += pnode->off;
            }
        }
    }

    return ERR_FAILED;
}
size_t buffer_size(struct buffer_ctx *pctx)
{
    return pctx->total_len;
}
uint32_t buffer_get_iov(struct buffer_ctx *pctx, size_t uiatmost,
    IOV_TYPE *piov, const uint32_t uicnt)
{
    if (uiatmost > pctx->total_len)
    {
        uiatmost = pctx->total_len;
    }
    if (0 == uiatmost)
    {
        return 0;
    }

    uint32_t index = 0;
    struct bufnode_ctx *pnode = pctx->head;
    while (NULL != pnode
        && index < uicnt
        && uiatmost > 0) 
    {
        piov[index].IOV_PTR_FIELD = (void *)(pnode->buffer + pnode->misalign);
        if (uiatmost >= pnode->off)
        {
            piov[index].IOV_LEN_FIELD = (IOV_LEN_TYPE)pnode->off;
            uiatmost -= pnode->off;
        }
        else
        {
            piov[index].IOV_LEN_FIELD = (IOV_LEN_TYPE)uiatmost;
            uiatmost = 0;
        }
        index++;

        pnode = pnode->next;
    }

    return index;
}
uint32_t buffer_write_iov_application(struct buffer_ctx *pbuf, const size_t uisize,
    IOV_TYPE *piov, const uint32_t uiovcnt)
{
    buffer_lock(pbuf);
    ASSERTAB(0 == pbuf->freeze_write, "buffer tail already freezed.");
    pbuf->freeze_write = 1;
    uint32_t uicoun = buffer_expand_iov(pbuf, uisize, piov, uiovcnt);
    buffer_unlock(pbuf);
    return uicoun;
}
void buffer_write_iov_commit(struct buffer_ctx *pbuf, size_t ilens,
    IOV_TYPE *piov, const uint32_t uiovcnt)
{
    buffer_lock(pbuf);
    ASSERTAB(0 != pbuf->freeze_write, "buffer tail already unfreezed.");
    buffer_commit_iov(pbuf, ilens, piov, uiovcnt);
    pbuf->freeze_write = 0;
    buffer_unlock(pbuf);
}
uint32_t buffer_read_iov_application(struct buffer_ctx *pbuf, size_t uisize,
    IOV_TYPE *piov, const uint32_t uiovcnt)
{
    buffer_lock(pbuf);
    ASSERTAB(0 == pbuf->freeze_read, "buffer head already freezed.");
    uint32_t uicnt = buffer_get_iov(pbuf, uisize, piov, uiovcnt);
    if (uicnt > 0)
    {
        pbuf->freeze_read = 1;
    }
    buffer_unlock(pbuf);
    return uicnt;
}
void buffer_read_iov_commit(struct buffer_ctx *pbuf, size_t uisize)
{
    buffer_lock(pbuf);
    ASSERTAB(1 == pbuf->freeze_read, "buffer head already unfreezed.");
    if (uisize > 0)
    {
        buffer_drain(pbuf, uisize);
    }
    pbuf->freeze_read = 0;
    buffer_unlock(pbuf);
}
