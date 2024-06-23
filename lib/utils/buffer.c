#include "utils/buffer.h"
#include "utils/utils.h"
#include "utils/netutils.h"

typedef struct bufnode_ctx {
    int32_t used;
    struct bufnode_ctx *next;
    char *buffer;
    size_t buffer_lens;
    size_t misalign;
    size_t off;
}bufnode_ctx;

#define MAX_COPY_IN_EXPAND       4096
#define MAX_REALIGN_IN_EXPAND    2048
#define FIRST_FORMAT_IN_EXPAND   256
#define NODE_SPACE_PTR(ch) ((ch)->buffer + (ch)->misalign + (ch)->off)
#define NODE_SPACE_LEN(ch) ((ch)->buffer_lens - ((ch)->misalign + (ch)->off))
#define RECOED_IOV(ch, lens) \
    iov[index].IOV_PTR_FIELD = NODE_SPACE_PTR(ch);\
    iov[index].IOV_LEN_FIELD = (IOV_LEN_TYPE)lens;\
    ch->used = 1;\
    index++

//新建一节点
static bufnode_ctx *_node_new(const size_t size) {
    size_t total = ROUND_UP(size + sizeof(bufnode_ctx), sizeof(void *) < 8 ? 512 : ONEK);
    //size_t total = size + sizeof(bufnode_ctx);
    char *buf;
    MALLOC(buf, total);
    bufnode_ctx *node = (bufnode_ctx *)buf;
    ZERO(node, sizeof(bufnode_ctx));
    node->buffer_lens = total - sizeof(bufnode_ctx);
    node->buffer = (char *)(node + 1);
    return node;
}
//通过调整是否足够
static int32_t _should_realign(bufnode_ctx *node, const size_t lens) {
    return node->buffer_lens - node->off >= lens &&
        (node->off < node->buffer_lens / 2) &&
        (node->off <= MAX_REALIGN_IN_EXPAND);
}
//调整
static void _align(bufnode_ctx *node) {
    memmove(node->buffer, node->buffer + node->misalign, node->off);
    node->misalign = 0;
}
//释放pnode及后面节点
static void _free_all_node(bufnode_ctx *node) {
    bufnode_ctx *pnext;
    for (; NULL != node; node = pnext) {
        pnext = node->next;
        FREE(node);
    }
}
//pnode 及后面节点是否为空
static int32_t _node_all_empty(bufnode_ctx *node) {
    for (; NULL != node; node = node->next) {
        if (0 != node->off) {
            return 0;
        }
    }
    return 1;
}
//释放空节点
static bufnode_ctx **_free_trailing_empty_node(buffer_ctx *ctx) {
    bufnode_ctx **node = ctx->tail_with_data;
    while (NULL != (*node) 
        && (*node)->off != 0) {
        node = &(*node)->next;
    }
    if (NULL != *node) {
        ASSERTAB(_node_all_empty(*node), "node must empty.");
        _free_all_node(*node);
        *node = NULL;
    }
    return node;
}
//插入,会清理空节点
static void _node_insert(buffer_ctx *ctx, bufnode_ctx *node) {
    if (NULL == *ctx->tail_with_data) {
        ASSERTAB(ctx->tail_with_data == &ctx->head, "tail_with_data not equ head.");
        ASSERTAB(ctx->head == NULL, "head not NULL.");
        ctx->head = ctx->tail = node;
    } else {
        bufnode_ctx **last = _free_trailing_empty_node(ctx);
        *last = node;
        if (0 != node->off) {
            ctx->tail_with_data = last;
        }
        ctx->tail = node;
    }
    ctx->total_lens += node->off;
}
//新建节点并插入
static bufnode_ctx *_node_insert_new(buffer_ctx *ctx, const size_t lens) {
    bufnode_ctx *pnode = _node_new(lens);
    _node_insert(ctx, pnode);
    return pnode;
}
static void _last_with_data(buffer_ctx *ctx) {
    bufnode_ctx **node = ctx->tail_with_data;
    if (NULL == *node) {
        return;
    }
    while (NULL != (*node)->next) {
        node = &(*node)->next;
        if (0 != (*node)->off) {
            ctx->tail_with_data = node;
        }
    }
}
//扩展空间，保证连续内存 used 外部设定
static bufnode_ctx *_buffer_expand_single(buffer_ctx *ctx, const size_t lens) {
    bufnode_ctx *node, **last;
    last = ctx->tail_with_data;
    //最后一个有数据的节点满的
    if (NULL != *last 
        && 0 == NODE_SPACE_LEN(*last)) {
        last = &(*last)->next;
    }
    node = *last;
    if (NULL == node) {
        return _node_insert_new(ctx, lens);
    }
    if (NODE_SPACE_LEN(node) >= lens) {
        return node;
    }
    if (0 == node->off) {
        //插入新的，并删除pnode
        return _node_insert_new(ctx, lens);
    }
    //调整
    if (_should_realign(node, lens)) {
        _align(node);
        return node;
    }
    //空闲空间小于总空间的1/8 或者 已有的数据量大于MAX_COPY_IN_EXPAND(4096)
    if (NODE_SPACE_LEN(node) < node->buffer_lens / 8 
        || node->off > MAX_COPY_IN_EXPAND) {
        if (NULL != node->next 
            && NODE_SPACE_LEN(node->next) >= lens) {
            return node->next;
        } else {
            return _node_insert_new(ctx, lens);
        }
    }
    //数据迁移
    bufnode_ctx *tmp = _node_new(node->off + lens);
    tmp->off = node->off;
    memcpy(tmp->buffer, node->buffer + node->misalign, node->off);
    ASSERTAB(*last == node, "tail_with_data not equ pnode.");
    *last = tmp;
    if (ctx->tail == node) {
        ctx->tail = tmp;
    }
    tmp->next = node->next;
    FREE(node);
    return tmp;
}
static uint32_t _buffer_expand(buffer_ctx *ctx, const size_t lens, IOV_TYPE *iov, const uint32_t cnt) {
    bufnode_ctx *tmp, *next, *node = ctx->tail;
    size_t avail, remain, used, space;
    uint32_t index = 0;
    ASSERTAB(cnt >= 2, "param error.");
    if (NULL == node) {
        node = _node_new(lens);
        _node_insert(ctx, node);
        RECOED_IOV(node, lens);
        return index;
    }
    used = 0; //使用了多少个节点
    avail = 0;//可用空间
    for (node = *ctx->tail_with_data; NULL != node; node = node->next) {
        if (0 != node->off) {
            space = (size_t)NODE_SPACE_LEN(node);
            ASSERTAB(node == *ctx->tail_with_data, "ail_with_data not equ pnode.");
            if (0 != space) {
                avail += space;
                ++used;
                RECOED_IOV(node, space);
            }
        } else {
            node->misalign = 0;
            avail += node->buffer_lens;
            ++used;
            RECOED_IOV(node, node->buffer_lens);
        }
        if (avail >= lens) {
            return index;
        }
        if (used == cnt) {
            break;
        }
    }
    //没有达到最大节点数，空间还不够
    if (used < cnt) {
        remain = lens - avail;
        ASSERTAB(NULL == node, "pnode not equ NULL.");
        tmp = _node_new(remain);
        ctx->tail->next = tmp;
        ctx->tail = tmp;
        RECOED_IOV(tmp, remain);
        return index;
    }
    //所有节点都不能装下
    index = 0;
    int32_t delall = 0;
    node = *ctx->tail_with_data;
    if (0 == node->off) {//无数据
        ASSERTAB(node == ctx->head, "head not equ pnode.");
        delall = 1;
        avail = 0;
    } else {
        avail = (size_t)NODE_SPACE_LEN(node);
        if (0 != avail) {
            RECOED_IOV(node, avail);
        }        
        node = node->next;        
    }
    //释放
    for (; NULL != node; node = next) {
        next = node->next;
        ASSERTAB(0 == node->off, "node not empty.");
        FREE(node);
    }
    ASSERTAB(lens >= avail, "logic error.");
    remain = lens - avail;
    tmp = _node_new(remain);
    RECOED_IOV(tmp, remain);
    if (delall) {
        ctx->head = ctx->tail = tmp;
        ctx->tail_with_data = &ctx->head;
    } else {
        (*ctx->tail_with_data)->next = tmp;
        ctx->tail = tmp;
    }
    return index;
}
//cnt _buffer_expand_iov 返回数量
static size_t _buffer_commit_expand(buffer_ctx *ctx, size_t lens, IOV_TYPE *iov, const uint32_t cnt) {
    if (0 == cnt) {
        return 0;
    }
    //只有一个
    if (1 == cnt
        && NULL != ctx->tail
        && iov[0].IOV_PTR_FIELD == (void *)NODE_SPACE_PTR(ctx->tail)) {
        ASSERTAB(lens <= (size_t)NODE_SPACE_LEN(ctx->tail), "logic error.");
        ctx->tail->used = 0;
        ctx->tail->off += lens;
        ctx->total_lens += lens;
        if (0 != lens) {
            _last_with_data(ctx);
        }
        return lens;
    }
    uint32_t i;
    bufnode_ctx *node, **first, **fill;
    first = ctx->tail_with_data;
    if (NULL == *first) {
        return 0;
    }
    if (0 == NODE_SPACE_LEN(*first)) {
        first = &(*first)->next;
    }
    //检查
    node = *first;
    for (i = 0; i < cnt; ++i) {
        if (NULL == node) {
            return 0;
        }
        if (iov[i].IOV_PTR_FIELD != (void *)NODE_SPACE_PTR(node)) {
            return 0;
        }
        node = node->next;
    }
    //填充
    size_t added = 0;
    fill = first;
    for (i = 0; i < cnt; ++i) {
        (*fill)->used = 0;
        if (lens > 0) {
            if (lens >= iov[i].IOV_LEN_FIELD) {
                (*fill)->off += iov[i].IOV_LEN_FIELD;
                added += iov[i].IOV_LEN_FIELD;
                lens -= iov[i].IOV_LEN_FIELD;
            } else {
                (*fill)->off += lens;
                added += lens;
                lens = 0;
            }
        }
        if ((*fill)->off > 0) {
            ctx->tail_with_data = fill;
        }
        fill = &(*fill)->next;
    }
    ASSERTAB(0 == lens, "logic error.");
    ctx->total_lens += added;
    return added;
}
void buffer_init(buffer_ctx *ctx) {
    ZERO(ctx, sizeof(buffer_ctx));
    ctx->tail_with_data = &ctx->head;
}
void buffer_free(buffer_ctx *ctx) {
    _free_all_node(ctx->head);
}
size_t buffer_size(buffer_ctx *ctx) {
    return ctx->total_lens;
}
int32_t buffer_append(buffer_ctx *ctx, void *data, const size_t lens) {
    ASSERTAB(0 == ctx->freeze_write, "write freezed");
    if (0 == lens
        || NULL == data) {
        return ERR_OK;
    }
    char *tmp = (char*)data;
    IOV_TYPE iov[MAX_EXPAND_NIOV];
    size_t remain = lens;
    size_t i, off = 0;
    uint32_t num = _buffer_expand(ctx, lens, iov, MAX_EXPAND_NIOV);
    for (i = 0; i < num && remain > 0; i++) {
        if ((IOV_LEN_TYPE)remain >= iov[i].IOV_LEN_FIELD) {
            memcpy(iov[i].IOV_PTR_FIELD, tmp + off, iov[i].IOV_LEN_FIELD);
            off += iov[i].IOV_LEN_FIELD;
            remain -= iov[i].IOV_LEN_FIELD;
        } else {
            memcpy(iov[i].IOV_PTR_FIELD, tmp + off, remain);
            iov[i].IOV_LEN_FIELD = (IOV_LEN_TYPE)remain;
            remain = 0;
        }
    }
    ASSERTAB(lens == _buffer_commit_expand(ctx, lens, iov, num), "commit lens not equ buffer lens.");
    return ERR_OK;
}
int32_t buffer_appendv(buffer_ctx *ctx, const char *fmt, ...) {
    ASSERTAB(0 == ctx->freeze_write, "write freezed");
    va_list va;
    int32_t rtn, size;
    bufnode_ctx *node = _buffer_expand_single(ctx, FIRST_FORMAT_IN_EXPAND);
    node->used = 1;
    va_start(va, fmt);
    while (1) {
        size = (int32_t)NODE_SPACE_LEN(node);
        rtn = vsnprintf(NODE_SPACE_PTR(node), (size_t)size, fmt, va);
        if (rtn < 0) {
            node->used = 0;
            va_end(va);
            return ERR_FAILED;
        }
        if (rtn >= 0
            && rtn < size) {
            node->used = 0;
            node->off += rtn;
            ctx->total_lens += rtn;
            break;
        }
        node->used = 0;
        node = _buffer_expand_single(ctx, rtn + 1);
        node->used = 1;
    }
    va_end(va);
    return ERR_OK;
}
static bufnode_ctx *_search_start(bufnode_ctx *node, size_t start, size_t *totaloff) {
    while (NULL != node
        && 0 != node->off) {
        *totaloff += node->off;
        //到达开始位置的节点
        if (*totaloff > start) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}
int32_t buffer_copyout(buffer_ctx *ctx, const size_t start, void *out, size_t lens) {
    ASSERTAB(0 == ctx->freeze_read, "read freezed");
    if (start >= ctx->total_lens 
        || 0 == lens) {
        return 0;
    }
    size_t remain = ctx->total_lens - start;
    if (lens > remain) {
        lens = remain;
    }
    bufnode_ctx *node;
    char *data = out;
    size_t nread = lens;
    if (0 == start) {
        node = ctx->head;
    } else {
        size_t off = 0;
        node = _search_start(ctx->head, start, &off);
        off = node->off - (off - start);
        if (off > 0) {
            remain = node->off - off;
            if (lens > remain) {
                memcpy(data, node->buffer + node->misalign + off, remain);
                data += remain;
                lens -= remain;
                node = node->next;
            } else {
                memcpy(data, node->buffer + node->misalign + off, lens);
                return (int32_t)nread;
            }
        }
    }
    while (0 != lens
        && lens >= node->off) {
        memcpy(data, node->buffer + node->misalign, node->off);
        data += node->off;
        lens -= node->off;
        node = node->next;
    }
    if (0 != lens) {
        memcpy(data, node->buffer + node->misalign, lens);
    }
    return (int32_t)nread;
}
int32_t buffer_drain(buffer_ctx *ctx, size_t lens) {
    ASSERTAB(0 == ctx->freeze_read, "read freezed");
    bufnode_ctx *node, *next;
    size_t remain, oldlen;
    oldlen = ctx->total_lens;
    if (0 == oldlen) {
        return 0;
    }
    if (lens > oldlen) {
        lens = oldlen;
    }
    ctx->total_lens -= lens;
    remain = lens;
    for (node = ctx->head; NULL != node && remain >= node->off; node = next) {
        next = node->next;
        remain -= node->off;
        if (node == *ctx->tail_with_data) {
            ctx->tail_with_data = &ctx->head;
        }
        if (&node->next == ctx->tail_with_data) {
            ctx->tail_with_data = &ctx->head;
        }
        if (0 == node->used) {
            FREE(node);
        } else {
            ASSERTAB(0 == remain, "logic error.");
            node->misalign += node->off;
            node->off = 0;
            break;
        }
    }
    ctx->head = node;
    if (NULL != node) {
        ASSERTAB(remain <= node->off, "logic error.");
        node->misalign += remain;
        node->off -= remain;
    } else {
        ctx->head = ctx->tail = NULL;
        ctx->tail_with_data = &(ctx)->head;
    }
    return (int32_t)lens;
}
int32_t buffer_remove(buffer_ctx *ctx, void *out, size_t lens) {
    int32_t rtn = buffer_copyout(ctx, 0, out, lens);
    if (rtn > 0) {
        ASSERTAB(rtn == buffer_drain(ctx, rtn), "drain lens not equ copy lens.");
    }
    return rtn;
}
static int32_t _search_memcmp(bufnode_ctx *node, cmp_func cmp, size_t off, char *what, size_t wlen) {
    size_t ncomp;
    while (wlen > 0
        && NULL != node) {
        ncomp = wlen + off > node->off ? node->off - off : wlen;
        if (0 != cmp(node->buffer + node->misalign + off, what, ncomp)) {
            return ERR_FAILED;
        }
        what += ncomp;
        wlen -= ncomp;
        off = 0;
        node = node->next;
    }
    return ERR_OK;
}
int32_t buffer_search(buffer_ctx *ctx, const int32_t ncs,
    const size_t start, size_t end, char *what, size_t wlens) {
    ASSERTAB(0 == ctx->freeze_read, "read freezed");
    if (0 == ctx->total_lens) {
        return ERR_FAILED;
    }
    if (0 == end || end >= ctx->total_lens) {
        end = ctx->total_lens;
    } else {
        end++;
    }
    if (start + wlens > end) {
        return ERR_FAILED;
    }
    chr_func chr;
    cmp_func cmp;
    if (0 == ncs) {
        chr = memchr;
        cmp = memcmp;
    } else {
        chr = memichr;
        cmp = _memicmp;
    }
    //查找开始位置所在节点
    size_t totaloff = 0;
    bufnode_ctx *node = _search_start(ctx->head, start, &totaloff);
    ASSERTAB(NULL != node && 0 != node->off, "can't search start node.");
    char *pschar, *pstart;
    size_t uioff = node->off - (totaloff - start);
    while (NULL != node
        && 0 != node->off) {
        if (totaloff - node->off + uioff + wlens > end) {
            break;
        }
        pstart = node->buffer + node->misalign + uioff;
        pschar = (char *)chr(pstart, what[0], node->off - uioff);
        if (NULL != pschar) {
            uioff += (pschar - pstart);
            if (totaloff - node->off + uioff + wlens > end) {
                break;
            }
            if (ERR_OK == _search_memcmp(node, cmp, uioff, what, wlens)) {
                return (int32_t)(totaloff - node->off + uioff);
            }
            uioff++;
            if (node->off == uioff) {
                uioff = 0;
                node = node->next;
                if (NULL != node) {
                    totaloff += node->off;
                }
            }
        } else {
            uioff = 0;
            node = node->next;
            if (NULL != node) {
                totaloff += node->off;
            }
        }
    }
    return ERR_FAILED;
}
char buffer_at(buffer_ctx *ctx, size_t pos) {
    ASSERTAB(0 == ctx->freeze_read, "read freezed");
    ASSERTAB(pos < ctx->total_lens, "index error.");
    size_t off = 0;
    bufnode_ctx *node = _search_start(ctx->head, pos, &off);
    off = node->off - (off - pos);
    return (node->buffer + node->misalign + off)[0];
}
uint32_t buffer_expand(buffer_ctx *ctx, const size_t lens, IOV_TYPE *iov, const uint32_t cnt) {
    ASSERTAB(0 == ctx->freeze_write, "write freezed");
    ctx->freeze_write = 1;
    return _buffer_expand(ctx, lens, iov, cnt);
}
void buffer_commit_expand(buffer_ctx *ctx, size_t lens, IOV_TYPE *iov, const uint32_t cnt) {
    ASSERTAB(1 == ctx->freeze_write, "write unfreezed");    
    ASSERTAB(lens == _buffer_commit_expand(ctx, lens, iov, cnt), "commit error.");
    ctx->freeze_write = 0;
}
uint32_t buffer_get(buffer_ctx *ctx, size_t atmost, IOV_TYPE *iov, const uint32_t cnt) {
    ASSERTAB(0 == ctx->freeze_read, "read freezed");
    ctx->freeze_read = 1;
    if (atmost > ctx->total_lens) {
        atmost = ctx->total_lens;
    }
    if (0 == atmost) {
        return 0;
    }
    uint32_t index = 0;
    bufnode_ctx *node = ctx->head;
    while (NULL != node
        && index < cnt
        && atmost > 0) {
        iov[index].IOV_PTR_FIELD = (void *)(node->buffer + node->misalign);
        if (atmost >= node->off) {
            iov[index].IOV_LEN_FIELD = (IOV_LEN_TYPE)node->off;
            atmost -= node->off;
        } else {
            iov[index].IOV_LEN_FIELD = (IOV_LEN_TYPE)atmost;
            atmost = 0;
        }
        index++;
        node = node->next;
    }
    return index;
}
void buffer_commit_get(buffer_ctx *ctx, size_t lens) {
    ASSERTAB(1 == ctx->freeze_read, "read unfreezed.");
    if (lens > 0) {
        buffer_drain(ctx, lens);
    }
    ctx->freeze_read = 0;
}
int32_t buffer_from_sock(buffer_ctx *ctx, SOCKET fd, size_t *nread,
    int32_t(*_readv)(SOCKET, IOV_TYPE *, uint32_t, void *, size_t *), void *arg) {
    *nread = 0;
    int32_t nbuf = sock_nread(fd);
    if (ERR_FAILED == nbuf) {
        return ERR_FAILED;
    }
    if (0 == nbuf
        || nbuf > MAX_RECV_SIZE) {
        nbuf = MAX_RECV_SIZE;
    }
    size_t readed;
    int32_t rtn;
    uint32_t niov;
    IOV_TYPE iov[MAX_EXPAND_NIOV];
    for (;;) {
        niov = buffer_expand(ctx, (size_t)nbuf, iov, MAX_EXPAND_NIOV);
        rtn = _readv(fd, iov, niov, arg, &readed);
        buffer_commit_expand(ctx, readed, iov, niov);
        *nread += readed;
        if (ERR_FAILED == rtn) {
            break;
        }
        if (0 == readed) {
            break;
        }
#ifdef READV_EINVAL
        if (readed < (size_t)nbuf
            || *nread >= (size_t)nbuf) {
            break;
        }
#endif
    }
    return rtn;
}
