#include "utils/buffer.h"
#include "utils/utils.h"
#include "utils/netutils.h"

//|              |   misalign   |    off    |           | 
//|--------------|--------------|-----------|-----------|
//|node          |buffer                                |
typedef struct bufnode_ctx {
    int32_t used;           //是否被分散读写操作锁定（非零时不可释放）
    struct bufnode_ctx *next;
    char *buffer;           //实际数据缓冲区指针
    free_cb _free;          //外部数据的释放函数（零拷贝时使用）
    size_t buffer_lens;     //buffer 总容量
    size_t misalign;        //已读取（消耗）的字节数（左偏移）
    size_t off;             //已写入的有效数据长度
}bufnode_ctx;

#define MAX_COPY_IN_EXPAND       4096 //节点数据量超过此值时不做数据迁移，直接新建节点
#define MAX_REALIGN_IN_EXPAND    2048 //节点 off 不超过此值时允许通过对齐操作复用空间
#define FIRST_FORMAT_IN_EXPAND   256  //格式化写入时首次预分配的空间大小
#define NODE_SPACE_PTR(ch) ((ch)->buffer + (ch)->misalign + (ch)->off)  //节点空闲区起始指针
#define NODE_SPACE_LEN(ch) ((ch)->buffer_lens - ((ch)->misalign + (ch)->off)) //节点空闲区长度
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
static void _node_free(bufnode_ctx *node) {
    if (NULL != node->_free) {
        node->_free(node->buffer);
    }
    FREE(node);
}
//通过偏移判断是否足够
static int32_t _should_realign(bufnode_ctx *node, const size_t lens) {
    return node->buffer_lens - node->off >= lens &&
        (node->off < node->buffer_lens / 2) &&
        (node->off <= MAX_REALIGN_IN_EXPAND);
}
//对齐
static void _align(bufnode_ctx *node) {
    memmove(node->buffer, node->buffer + node->misalign, node->off);
    node->misalign = 0;
}
//释放pnode及其后续节点
static void _free_all_node(bufnode_ctx *node) {
    bufnode_ctx *pnext;
    for (; NULL != node; node = pnext) {
        pnext = node->next;
        _node_free(node);
    }
}
//pnode 及其后续节点是否为空
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
//插入,清理末尾空节点
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
// 更新 tail_with_data 指针，使其指向最后一个有数据的节点
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
//扩展空间，保证外部在 used 外部设定
static bufnode_ctx *_buffer_expand_single(buffer_ctx *ctx, const size_t lens) {
    bufnode_ctx *node, **last;
    last = ctx->tail_with_data;
    //找最后一个有数据的节点位置
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
        //是全新的，则删除pnode
        return _node_insert_new(ctx, lens);
    }
    //对齐
    if (_should_realign(node, lens)) {
        _align(node);
        return node;
    }
    //剩余空间小于总空间的1/8 或者 存在的数据量超过MAX_COPY_IN_EXPAND(4096)
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
    _node_free(node);
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
    //没有达到满节点，剩余空间还够
    if (used < cnt) {
        remain = lens - avail;
        ASSERTAB(NULL == node, "pnode not equ NULL.");
        tmp = _node_new(remain);
        ctx->tail->next = tmp;
        ctx->tail = tmp;
        RECOED_IOV(tmp, remain);
        return index;
    }
    //所有节点都装满了
    index = 0;
    int32_t delall = 0;
    node = *ctx->tail_with_data;
    if (0 == node->off) {//全新的
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
        _node_free(node);
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
//cnt _buffer_expand_iov 的数组数量
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
    //验证
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
void buffer_external(buffer_ctx *ctx, void *data, const size_t lens, free_cb _free) {
    bufnode_ctx *node;
    CALLOC(node, 1, sizeof(bufnode_ctx));
    node->buffer = (char *)data;
    node->_free = _free;
    node->buffer_lens = lens;
    node->off = lens;
    _node_insert(ctx, node);
}
int32_t buffer_append(buffer_ctx *ctx, void *data, const size_t lens) {
    ASSERTAB(0 == ctx->freeze_write, "write freezed");
    if (0 == lens
        || NULL == data) {
        return ERR_OK;
    }
    /* 快速路径：尾节点已有数据且未被分散读写锁定，剩余空间足够直接写入，
     * 跳过 expand/commit 流程，减少开销。 */
    bufnode_ctx *tail = ctx->tail;
    if (NULL != tail
        && 0 != tail->off
        && 0 == tail->used
        && NODE_SPACE_LEN(tail) >= lens) {
        memcpy(NODE_SPACE_PTR(tail), data, lens);
        tail->off         += lens;
        ctx->total_lens   += lens;
        return ERR_OK;
    }
    /* 对齐恢复路径：快路径因空间不足失败，但 tail 有 misalign 可回收。
     * _align 将已有数据前移（misalign→0），使空闲区连续，再做单次 memcpy，
     * 避免进入 _buffer_expand 的跨节点分散写流程。 */
    if (NULL != tail
        && 0 == tail->used
        && tail->misalign > 0
        && _should_realign(tail, lens)) {
        _align(tail);
        memcpy(NODE_SPACE_PTR(tail), data, lens);
        tail->off       += lens;
        ctx->total_lens += lens;
        return ERR_OK;
    }
    /* 慢速路径：走完整的 expand + commit 流程 */
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
    va_list tmp;
    va_start(va, fmt);
    while (1) {
        size = (int32_t)NODE_SPACE_LEN(node);
        va_copy(tmp, va);
        rtn = vsnprintf(NODE_SPACE_PTR(node), (size_t)size, fmt, tmp);
        va_end(tmp);
        if (rtn < 0) {
            node->used = 0;
            va_end(va);
            return ERR_FAILED;
        }
        if (rtn < size) {
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
// 从链表头开始线性遍历，找到包含 start 偏移量的节点
static bufnode_ctx *_search_start(bufnode_ctx *node, size_t start, size_t *totaloff) {
    while (NULL != node
        && 0 != node->off) {
        *totaloff += node->off;
        if (*totaloff > start) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}
// 带游标缓存的版本：若 start >= hint_base_off，直接从上次节点继续，
// 避免从 head 线性遍历；命中后更新游标供下次使用
static bufnode_ctx *_search_start_cached(buffer_ctx *ctx, size_t start, size_t *totaloff) {
    bufnode_ctx *node;
    if (NULL != ctx->hint_node && start >= ctx->hint_base_off) {
        node = ctx->hint_node;
        *totaloff = ctx->hint_base_off;
    } else {
        node = ctx->head;
        *totaloff = 0;
    }
    while (NULL != node && 0 != node->off) {
        *totaloff += node->off;
        if (*totaloff > start) {
            ctx->hint_node     = node;
            ctx->hint_base_off = *totaloff - node->off;
            return node;
        }
        node = node->next;
    }
    return NULL;
}
size_t buffer_copyout(buffer_ctx *ctx, const size_t start, void *out, size_t lens) {
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
        node = _search_start_cached(ctx, start, &off);
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
                return nread;
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
    return nread;
}
size_t buffer_drain(buffer_ctx *ctx, size_t lens) {
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
    /* 在 drain 循环释放节点前保存游标，drain 后再恢复（节点存活）或清零（节点已释放）。 */
    bufnode_ctx *saved_hint     = ctx->hint_node;
    size_t       saved_hint_off = ctx->hint_base_off;
    ctx->hint_node     = NULL;
    ctx->hint_base_off = 0;
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
            _node_free(node);
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
    /* 恢复搜索游标：
     *   saved_hint_off >= lens  → 游标节点未被释放，基偏移减去 lens 即可。
     *   ctx->head == saved_hint → drain 停在游标节点内部（已成为新 head），
     *                             将基偏移重置为 0。
     *   其他情况表示游标节点已被释放，游标保持 NULL/0。 */
    if (NULL != saved_hint) {
        if (saved_hint_off >= lens) {
            ctx->hint_node     = saved_hint;
            ctx->hint_base_off = saved_hint_off - lens;
        } else if (ctx->head == saved_hint) {
            ctx->hint_node     = saved_hint;
            ctx->hint_base_off = 0;
        }
    }
    return lens;
}
size_t buffer_remove(buffer_ctx *ctx, void *out, size_t lens) {
    size_t rtn = buffer_copyout(ctx, 0, out, lens);
    if (rtn > 0) {
        ASSERTAB(rtn == buffer_drain(ctx, rtn), "drain lens not equ copy lens.");
    }
    return rtn;
}
// 跨节点比较数据，从 node 的 off 偏移处与 what 比较 wlen 字节
static int32_t _search_memcmp(bufnode_ctx *node, cmp_func cmp, size_t off, char *what, size_t wlen) {
    size_t ncomp;
    while (wlen > 0
        && NULL != node) {
        if (off >= node->off) {
            return ERR_FAILED;
        }
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
    bufnode_ctx *node = _search_start_cached(ctx, start, &totaloff);
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
    bufnode_ctx *node = _search_start_cached(ctx, pos, &off);
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
