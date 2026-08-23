#include "utils/buffer.h"
#include "utils/utils.h"

#define MAX_COPY_IN_EXPAND       4096 //节点数据量超过此值时不做数据迁移，直接新建节点
#define MAX_REALIGN_IN_EXPAND    2048 //节点 off 不超过此值时允许通过对齐操作复用空间
#define FIRST_FORMAT_IN_EXPAND   256 //格式化写入时首次预分配的空间大小
#define NODE_SPACE_PTR(ch) ((ch)->buffer + (ch)->misalign + (ch)->off) //节点空闲区起始指针
#define NODE_SPACE_LEN(ch) ((ch)->buffer_lens - ((ch)->misalign + (ch)->off)) //节点空闲区长度
#define RECOED_IOV(ch, lens) \
    iov[index].IOV_PTR_FIELD = NODE_SPACE_PTR(ch);\
    iov[index].IOV_LEN_FIELD = (IOV_LEN_TYPE)lens;\
    ch->used = 1;\
    index++
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

//新建一节点
static bufnode_ctx *_buffer_node_new(const size_t size) {
    size_t align = sizeof(void *) < 8 ? 512 : ONEK;
    ASSERTAB(size <= SIZE_MAX - sizeof(bufnode_ctx) - (align - 1), "buffer node size overflow");
    size_t total = ROUND_UP(size + sizeof(bufnode_ctx), align);
    char *buf;
    MALLOC(buf, total);
    bufnode_ctx *node = (bufnode_ctx *)buf;
    ZERO(node, sizeof(bufnode_ctx));
    node->buffer_lens = total - sizeof(bufnode_ctx);
    node->buffer = (char *)(node + 1);
    return node;
}
static void _buffer_node_free(bufnode_ctx *node) {
    if (NULL != node->_free) {
        node->_free(node->buffer);
    }
    FREE(node);
}
// 外部托管(零拷贝)节点判定:内部节点的 buffer 由 _buffer_node_new 与节点头一次分配、紧随其后,
// 外部节点的 buffer 指向调用方内存。不用 _free 判定——buffer_external 允许 ext_free 传 NULL
static inline int32_t _buffer_node_external(bufnode_ctx *node) {
    return node->buffer != (char *)(node + 1);
}
// 一个节点在 iov 登记里的贡献:返回可写字节数,*slot 置 1 表示它要占掉一条 iov。
// 带数据的节点计空闲区(空闲为 0 时不占 iov),空外部节点不可写但仍占一条,空内部节点整块可写。
// _buffer_expand 据此登记 iov、buffer_space 据此预估"不触发分配的最大 lens",两边必须同源——
// 一旦分歧, buffer_from_sock 要么少读一截要么白扩一个节点
static inline size_t _buffer_node_avail(bufnode_ctx *node, uint32_t *slot) {
    size_t space;
    if (0 != node->off) {
        space = (size_t)NODE_SPACE_LEN(node);
        *slot = (0 != space) ? 1 : 0;
        return space;
    }
    *slot = 1;
    return _buffer_node_external(node) ? 0 : node->buffer_lens;
}
//通过偏移判断是否足够。外部节点的 misalign 区属调用方内存,回收它等于改写调用方数据,一律拒绝
static inline int32_t _buffer_should_realign(bufnode_ctx *node, const size_t lens) {
    if (_buffer_node_external(node)) {
        return 0;
    }
    return node->buffer_lens - node->off >= lens &&
        (node->off < node->buffer_lens / 2) &&
        (node->off <= MAX_REALIGN_IN_EXPAND);
}
//对齐
static void _buffer_align(bufnode_ctx *node) {
    memmove(node->buffer, node->buffer + node->misalign, node->off);
    node->misalign = 0;
}
//释放pnode及其后续节点
static void _buffer_free_all_node(bufnode_ctx *node) {
    bufnode_ctx *pnext;
    for (; NULL != node; node = pnext) {
        pnext = node->next;
        _buffer_node_free(node);
    }
}
//pnode 及其后续节点是否为空
static int32_t _buffer_node_all_empty(bufnode_ctx *node) {
    for (; NULL != node; node = node->next) {
        if (0 != node->off) {
            return 0;
        }
    }
    return 1;
}
//释放空节点
static bufnode_ctx **_buffer_free_trailing_empty_node(buffer_ctx *ctx) {
    bufnode_ctx **node = ctx->tail_with_data;
    while (NULL != (*node) 
        && (*node)->off != 0) {
        node = &(*node)->next;
    }
    if (NULL != *node) {
        ASSERTAB(_buffer_node_all_empty(*node), "node must empty.");
        _buffer_free_all_node(*node);
        *node = NULL;
    }
    return node;
}
//插入,清理末尾空节点
static void _buffer_node_insert(buffer_ctx *ctx, bufnode_ctx *node) {
    if (NULL == *ctx->tail_with_data) {
        ASSERTAB(ctx->tail_with_data == &ctx->head, "tail_with_data not equ head.");
        ASSERTAB(ctx->head == NULL, "head not NULL.");
        ctx->head = ctx->tail = node;
    } else {
        bufnode_ctx **last = _buffer_free_trailing_empty_node(ctx);
        *last = node;
        if (0 != node->off) {
            ctx->tail_with_data = last;
        }
        ctx->tail = node;
    }
    ctx->total_lens += node->off;
}
//新建节点并插入
static bufnode_ctx *_buffer_node_insert_new(buffer_ctx *ctx, const size_t lens) {
    bufnode_ctx *pnode = _buffer_node_new(lens);
    _buffer_node_insert(ctx, pnode);
    return pnode;
}
// 更新 tail_with_data 指针，使其指向最后一个有数据的节点
static void _buffer_last_with_data(buffer_ctx *ctx) {
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
        return _buffer_node_insert_new(ctx, lens);
    }
    if (NODE_SPACE_LEN(node) >= lens) {
        return node;
    }
    if (0 == node->off) {
        //是全新的，则删除pnode
        return _buffer_node_insert_new(ctx, lens);
    }
    //对齐
    if (_buffer_should_realign(node, lens)) {
        _buffer_align(node);
        return node;
    }
    //剩余空间小于总空间的1/8 或者 存在的数据量超过MAX_COPY_IN_EXPAND(4096)
    if (NODE_SPACE_LEN(node) < node->buffer_lens / 8 
        || node->off > MAX_COPY_IN_EXPAND) {
        if (NULL != node->next 
            && NODE_SPACE_LEN(node->next) >= lens) {
            return node->next;
        } else {
            return _buffer_node_insert_new(ctx, lens);
        }
    }
    //数据迁移
    bufnode_ctx *tmp = _buffer_node_new(node->off + lens);
    tmp->off = node->off;
    memcpy(tmp->buffer, node->buffer + node->misalign, node->off);
    ASSERTAB(*last == node, "tail_with_data not equ pnode.");
    *last = tmp;
    if (ctx->tail == node) {
        ctx->tail = tmp;
    }
    tmp->next = node->next;
    if (ctx->hint_node == node) {
        ctx->hint_node = NULL;
        ctx->hint_base_off = 0;
    }
    ASSERTAB(0 == node->used, "node in use.");
    _buffer_node_free(node);
    return tmp;
}
// 外部托管节点必须占一条零长 iov 项而不能被跳过:_buffer_commit_expand 按"iov[i] 对应链上
// 第 i 个节点"逐位校验,只容许跳过首个零空间节点,中途少记一项后续节点即全部错位并 abort;
// 而记 buffer_lens 又会把调用方的外部缓冲当可写空间(排空后 off==0 时尤其致命)
static uint32_t _buffer_expand(buffer_ctx *ctx, const size_t lens, IOV_TYPE *iov, const uint32_t cnt) {
    bufnode_ctx *tmp, *next, *node = ctx->tail;
    size_t avail, remain, used, space;
    uint32_t slot;
    uint32_t index = 0;
    ASSERTAB(cnt >= 2, "param error.");
    if (NULL == node) {
        node = _buffer_node_new(lens);
        _buffer_node_insert(ctx, node);
        RECOED_IOV(node, lens);
        return index;
    }
    used = 0; //使用了多少个节点
    avail = 0;//可用空间
    for (node = *ctx->tail_with_data; NULL != node; node = node->next) {
        // 登记的字节数与槽位归 _buffer_node_avail 统管, 这里只补 expand 独有的副作用
        if (0 != node->off) {
            ASSERTAB(node == *ctx->tail_with_data, "tail_with_data not equ pnode.");
        } else if (!_buffer_node_external(node)) {
            node->misalign = 0; //空的内部节点整块可写, 先把前缩量清掉
        }
        space = _buffer_node_avail(node, &slot);
        if (0 != slot) {
            avail += space;
            ++used;
            RECOED_IOV(node, space);
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
        tmp = _buffer_node_new(remain);
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
    //释放。这些节点第一趟被 RECOED_IOV 置过 used, 但那批 iov 已随 index 归零整体作废、
    //调用方拿不到, 故此处释放不违反"外部持有 iov 的节点不可释放"
    for (; NULL != node; node = next) {
        next = node->next;
        ASSERTAB(0 == node->off, "node not empty.");
        _buffer_node_free(node);
    }
    ASSERTAB(lens >= avail, "logic error.");
    remain = lens - avail;
    tmp = _buffer_node_new(remain);
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
// 清掉 _buffer_expand 经 RECOED_IOV 打在节点上的 used 标记, 起点与"填充"循环一致。
// 校验失败的早退路径必须调它: 那些节点只在"填充"循环里才解锁, 直接 return 会让它们停在
// used=1, 而 buffer_drain 与节点回收都跳过 used 非零的节点, 等于这块内存再不释放。
// 调用方那句 ASSERTAB(lens == _buffer_commit_expand(...)) 兜不住: readed 为 0 时 0 == 0 静默通过
static void _buffer_unpin(bufnode_ctx **first, const uint32_t cnt) {
    bufnode_ctx **cur = first;
    for (uint32_t i = 0; i < cnt && NULL != *cur; ++i) {
        (*cur)->used = 0;
        cur = &(*cur)->next;
    }
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
            _buffer_last_with_data(ctx);
        }
        return lens;
    }
    uint32_t i;
    bufnode_ctx *node, **first, **fill;
    first = ctx->tail_with_data;
    // 这里无需 unpin: _buffer_expand 打 pin 也是从 *tail_with_data 起步的
    // (唯一的例外是它开头"缓冲全空"那条分支, 但那条 insert 完 *tail_with_data 必非 NULL),
    // 所以 *first 为 NULL 就意味着一个节点都没被 pin 过
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
            _buffer_unpin(first, cnt);
            return 0;
        }
        if (iov[i].IOV_PTR_FIELD != (void *)NODE_SPACE_PTR(node)) {
            _buffer_unpin(first, cnt);
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
    // 暂存期间释放会把调用方仍持有 iov 的节点一并释放, 且 buffer_init 随后抹掉标志、
    // 事后无迹可寻; 其余入口都对误用大声 abort, 这里不该是唯一静默的那个
    ASSERTAB(0 == ctx->freeze_read, "read freezed");
    ASSERTAB(0 == ctx->freeze_write, "write freezed");
    _buffer_free_all_node(ctx->head);
    // 必须复位:否则 head/tail/tail_with_data/hint_node 全指向已释放节点,
    // 重复调用即 double free,total_lens 也会让释放后的 buffer_size 报出旧字节数
    buffer_init(ctx);
}
size_t buffer_size(buffer_ctx *ctx) {
    return ctx->total_lens;
}
// 按 _buffer_node_avail 逐节点累加(与 _buffer_expand 同源, 差别只在这里不改节点状态),
// 同样受 cnt 条 iov 的上限约束, 故返回值恰是"expand 不会新建节点"的最大 lens
size_t buffer_space(buffer_ctx *ctx, const uint32_t cnt) {
    bufnode_ctx *node;
    size_t avail = 0;
    uint32_t slot;
    uint32_t used = 0;
    for (node = *ctx->tail_with_data; NULL != node && used < cnt; node = node->next) {
        avail += _buffer_node_avail(node, &slot);
        used += slot;
    }
    return avail;
}
void buffer_external(buffer_ctx *ctx, void *data, const size_t lens, free_cb ext_free) {
    ASSERTAB(0 == ctx->freeze_write, "write freezed");
    ASSERTAB(0 == ctx->freeze_read, "read freezed");
    if (0 == lens) {
        if (NULL != ext_free) {
            ext_free(data);
        }
        return;
    }
    bufnode_ctx *node;
    CALLOC(node, 1, sizeof(bufnode_ctx));
    node->buffer = (char *)data;
    node->_free = ext_free;
    node->buffer_lens = lens;
    node->off = lens;
    _buffer_node_insert(ctx, node);
}
int32_t buffer_append(buffer_ctx *ctx, void *data, const size_t lens) {
    ASSERTAB(0 == ctx->freeze_write, "write freezed");
    ASSERTAB(0 == ctx->freeze_read, "read freezed");
    if (0 == lens
        || NULL == data) {
        return ERR_OK;
    }
    /* 快速路径：尾节点已有数据且未被分散读写锁定时，直接单次 memcpy 写进去，跳过
     * expand/commit 流程。空间不够但有 misalign 可回收就先 _buffer_align 把数据前移
     * (misalign→0)；对齐后空闲区必然装得下——_buffer_should_realign 的首条就是它 */
    bufnode_ctx *tail = ctx->tail;
    if (NULL != tail
        && 0 != tail->off
        && 0 == tail->used) {
        size_t space = NODE_SPACE_LEN(tail);
        if (space < lens
            && tail->misalign > 0
            && _buffer_should_realign(tail, lens)) {
            _buffer_align(tail);
            space = NODE_SPACE_LEN(tail);
        }
        if (space >= lens) {
            memcpy(NODE_SPACE_PTR(tail), data, lens);
            tail->off += lens;
            ctx->total_lens += lens;
            return ERR_OK;
        }
    }
    /* 慢速路径：走完整的 expand + commit 流程 */
    char *tmp = (char*)data;
    IOV_TYPE iov[MAX_EXPAND_NIOV];
    size_t remain = lens;
    size_t i, off = 0;
    uint32_t num = _buffer_expand(ctx, lens, iov, MAX_EXPAND_NIOV);
    for (i = 0; i < num && remain > 0; i++) {
        // 比较放在 size_t 域内做:Windows 的 IOV_LEN_TYPE 是 32 位 ULONG,
        // 把 remain 窄化过去会在 lens > 4GiB 时截断成小值,进而按完整 remain 越界 memcpy
        if (remain >= (size_t)iov[i].IOV_LEN_FIELD) {
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
    ASSERTAB(0 == ctx->freeze_read, "read freezed");
    va_list va;
    int32_t rtn, size;
    bufnode_ctx *node = _buffer_expand_single(ctx, FIRST_FORMAT_IN_EXPAND);
    node->used = 1;
    va_list tmp;
    va_start(va, fmt);
    size_t space;
    while (1) {
        // 收窄前钳一下: 节点空闲区超 INT32_MAX 时直接转 int32_t 会变负数,
        // 再 (size_t)size 传给 vsnprintf 就是个天文数字
        space = NODE_SPACE_LEN(node);
        size = space > (size_t)INT32_MAX ? INT32_MAX : (int32_t)space;
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
            if (0 != rtn) {
                _buffer_last_with_data(ctx);
            }
            break;
        }
        node->used = 0;
        node = _buffer_expand_single(ctx, rtn + 1);
        node->used = 1;
    }
    va_end(va);
    return ERR_OK;
}
// 带游标缓存的版本：若 start >= hint_base_off，直接从上次节点继续，
// 避免从 head 线性遍历；命中后更新游标供下次使用
static bufnode_ctx *_buffer_search_start_cached(buffer_ctx *ctx, size_t start, size_t *totaloff) {
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
            ctx->hint_node = node;
            ctx->hint_base_off = *totaloff - node->off;
            return node;
        }
        node = node->next;
    }
    return NULL;
}
size_t buffer_copyout(buffer_ctx *ctx, const size_t start, void *out, size_t lens) {
    ASSERTAB(0 == ctx->freeze_read, "read freezed");
    if (start >= ctx->total_lens || 0 == lens) {
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
        node = _buffer_search_start_cached(ctx, start, &off);
        if (NULL == node) {
            return 0;
        }
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
        && NULL != node
        && lens >= node->off) {
        memcpy(data, node->buffer + node->misalign, node->off);
        data += node->off;
        lens -= node->off;
        node = node->next;
    }
    if (0 != lens && NULL != node) {
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
    bufnode_ctx *saved_hint = ctx->hint_node;
    size_t saved_hint_off = ctx->hint_base_off;
    ctx->hint_node = NULL;
    ctx->hint_base_off = 0;
    ctx->total_lens -= lens;
    remain = lens;
    for (node = ctx->head; NULL != node && remain >= node->off; node = next) {
        next = node->next;
        remain -= node->off;
        // 两条互斥: 前者命中后 tail_with_data 已是 &ctx->head, 后者的比较必假
        if (node == *ctx->tail_with_data
            || &node->next == ctx->tail_with_data) {
            ctx->tail_with_data = &ctx->head;
        }
        if (0 == node->used) {
            _buffer_node_free(node);
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
    /* 恢复搜索游标: 节点还在(saved_hint_off >= lens)就把基偏移减掉 lens; drain 停在游标
     * 节点内部时它已成新 head, 基偏移归 0——但 off 被清空的节点做不了游标(那种节点会让
     * _buffer_search_start_cached 直接返 NULL), 故要一并判 off。其余情况游标保持 NULL/0。*/
    if (NULL != saved_hint) {
        if (saved_hint_off >= lens) {
            ctx->hint_node = saved_hint;
            ctx->hint_base_off = saved_hint_off - lens;
        } else if (ctx->head == saved_hint
                   && 0 != saved_hint->off) {
            ctx->hint_node = saved_hint;
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
static int32_t _buffer_search_memcmp(bufnode_ctx *node, cmp_func cmp, size_t off, char *what, size_t wlen) {
    size_t ncomp;
    while (wlen > 0 && NULL != node) {
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
    if (EMPTYPTR(what, wlens)) {
        return ERR_FAILED;
    }
    if (0 == ctx->total_lens) {
        return ERR_FAILED;
    }
    if (0 == end || end >= ctx->total_lens) {
        end = ctx->total_lens;
    } else {
        end++;
    }
    // 拆两步比,start + wlens 在 start 接近 SIZE_MAX 时会回绕
    if (start >= end
        || wlens > end - start) {
        return ERR_FAILED;
    }
    chr_func chr;
    cmp_func cmp;
    mem_funcs_pick(ncs, &chr, &cmp);
    //查找开始位置所在节点
    size_t totaloff = 0;
    bufnode_ctx *node = _buffer_search_start_cached(ctx, start, &totaloff);
    ASSERTAB(NULL != node && 0 != node->off, "can't search start node.");
    char *pschar, *pstart;
    size_t hit;
    size_t uioff = node->off - (totaloff - start);
    while (NULL != node && 0 != node->off) {
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
            if (ERR_OK == _buffer_search_memcmp(node, cmp, uioff, what, wlens)) {
                hit = totaloff - node->off + uioff;
                // 返回类型是 int32_t, 装不下的位置只能报未找到: 截断会得到一个负数或
                // 别的位置, 而调用方普遍只判 ERR_FAILED, 别的负值会被当成有效下标用下去
                if (hit > (size_t)INT32_MAX) {
                    return ERR_FAILED;
                }
                return (int32_t)hit;
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
    bufnode_ctx *node = _buffer_search_start_cached(ctx, pos, &off);
    ASSERTAB(NULL != node, "index error.");
    off = node->off - (off - pos);
    return (node->buffer + node->misalign + off)[0];
}
uint32_t buffer_expand(buffer_ctx *ctx, const size_t lens, IOV_TYPE *iov, const uint32_t cnt) {
    ASSERTAB(0 == ctx->freeze_write, "write freezed");
    ASSERTAB(0 == ctx->freeze_read, "read freezed");
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
    ASSERTAB(0 == ctx->freeze_write, "write freezed");
    if (atmost > ctx->total_lens) {
        atmost = ctx->total_lens;
    }
    // cnt 为 0 时下面的循环一条都填不出, 必须在置位之前退出：否则返回 0 却已进入暂存态，
    // 调用方照契约不调 buffer_commit_get, freeze_read 再无人清, 后续读写全部断言失败
    if (0 == atmost
        || 0 == cnt) {
        return 0;
    }
    ctx->freeze_read = 1;
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
        node->used = 1;
        index++;
        node = node->next;
    }
    ctx->pinned_n = index;
    return index;
}
// used 须在 drain 之前清掉, 否则 drain 走"节点被锁定"分支把本该释放的节点留成零长节点。
// 按 buffer_get 记下的 pinned_n 精确解锁, 而不是"从 head 扫到第一个未锁定节点"——后者
// 只在锁定段恰好自 head 起连续时才等价, 一旦 buffer_get 将来跳过某个节点就会漏解锁
void buffer_commit_get(buffer_ctx *ctx, size_t lens) {
    ASSERTAB(1 == ctx->freeze_read, "read unfreezed.");
    bufnode_ctx *node = ctx->head;
    uint32_t i;
    for (i = 0; i < ctx->pinned_n && NULL != node; i++) {
        node->used = 0;
        node = node->next;
    }
    ctx->pinned_n = 0;
    ctx->freeze_read = 0;
    if (lens > 0) {
        buffer_drain(ctx, lens);
    }
}
int32_t buffer_from_sock(buffer_ctx *ctx, SOCKET fd, size_t *nread,
    int32_t(*_readv)(SOCKET, IOV_TYPE *, uint32_t, void *, size_t *), void *arg) {
    *nread = 0;
    size_t nbuf = MAX_RECV_SIZE;
    size_t readed;
    size_t space;
    int32_t rtn;
    uint32_t niov;
    IOV_TYPE iov[MAX_EXPAND_NIOV];
    for (;;) {
        niov = buffer_expand(ctx, nbuf, iov, MAX_EXPAND_NIOV);
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
        // 这道早退是给 readv 的(AIX 上无数据时它返 EINVAL)，只能用在裸 socket 读:
        // 未读数据留在内核 socket buffer 里，下一次可读事件会再来。
        // arg 非 NULL 表示另有一层缓冲(SSL)，那时不能早退，理由见 _evpub_sock_read_ssl
        if (NULL == arg
            && (readed < nbuf
                || *nread >= nbuf)) {
            break;
        }
#endif
        if (readed < nbuf) {
            // 加地板: 残余空间太小时, 万一确认轮真读到了数据, 为省一次 malloc(~100ns)
            // 反而要多付一次 readv 系统调用(~1-2us), 买卖倒挂
            space = buffer_space(ctx, MAX_EXPAND_NIOV);
            nbuf = (space >= MAX_RECV_SIZE / 4) ? space : MAX_RECV_SIZE;
        } else {
            nbuf = MAX_RECV_SIZE;
        }
    }
    return rtn;
}
