#include "hash_ring.h"
#include "utils/utils.h"
#include "crypt/xxhash.h"

/* 栈上分配的最大名称长度，避免堆分配。
 * 节点名通常为 host:port 短字符串，512 字节可覆盖绝大多数场景，
 * 超出时每个节点回退为一次堆分配。 */
#define NAME_STACK_LEN  512
/* 单节点虚拟副本数上限。实际用量在几十到几百，这里留了三个数量级余量。
 * 设上限是因为 nreplicas 来自上层调用方，离谱的值会让 items 数组要几十 GB */
#define MAX_REPLICAS    65536

typedef struct hash_ring_list {
    list_node lnode;                //slist 节点（UPCAST 复原）
    hash_ring_node *node;           //指向真实节点数据
} hash_ring_list;
typedef struct hash_ring_item {
    hash_ring_node *node;   //所属真实节点
    uint64_t digest;        //该虚拟节点的哈希值
} hash_ring_item;

// qsort 比较函数，按 digest 值升序排列哈希环节点
static int32_t _hash_ring_sort(const void *a, const void *b) {
    hash_ring_item *itema = *(hash_ring_item**)a, *itemb = *(hash_ring_item**)b;
    if (itema->digest < itemb->digest) {
        return -1;
    } else if (itema->digest > itemb->digest) {
        return 1;
    } else {
        return 0;
    }
}
void hash_ring_init(hash_ring_ctx *ring) {
    ZERO(ring, sizeof(hash_ring_ctx));
}
void hash_ring_free(hash_ring_ctx *ring) {
    if (NULL == ring) {
        return;
    }
    hash_ring_list *cur;
    list_foreach_safe(&ring->nodes, ln, tmp) {
        cur = UPCAST(ln, hash_ring_list, lnode);
        FREE(cur->node->name);
        FREE(cur->node);
        FREE(cur);
    }
    for (uint32_t i = 0; i < ring->nitems; i++) {
        FREE(ring->items[i]);
    }
    FREE(ring->items);
    hash_ring_init(ring);
}
// 环上位置取 xxh64 的完整 64 位：截成 32 位时副本一多就会撞位，撞上的两个副本谁在前由 qsort 随机决定
static inline uint64_t _hash_ring_hash(void *data, size_t lens) {
    return xxh64(data, lens, 0);
}
// 为节点生成所有虚拟副本（replica）并添加到 items 数组。副本名是"节点名-序号"：节点名只拷一次，
// 序号用 u64tostr 写十进制。产出必须与 "-%u" 逐字节相同——名字一变 digest 就变，环上位置全漂
static void _hash_ring_add_items(hash_ring_ctx *ring, hash_ring_node *node) {
    char name_stack[NAME_STACK_LEN];
    const size_t sufmax = 12;// 后缀最长：'-' 加 uint32 最多 10 位，再加 u64tostr 补的 '\0'
    char *name;
    size_t nlen;
    hash_ring_item *item;
    // 节点名加最长的后缀放不进栈缓冲才上堆，整个节点只分配这一次
    int32_t heap = node->lens + sufmax > NAME_STACK_LEN;
    // items 一次扩够本节点全部副本，循环里只往尾部填
    REALLOC(ring->items, ring->items, sizeof(hash_ring_item *) * ((size_t)ring->nitems + node->nreplicas));
    if (heap) {
        MALLOC(name, node->lens + sufmax);
    } else {
        name = name_stack;
    }
    memcpy(name, node->name, node->lens);// 节点名与 '-' 只写一次，每轮只改后面的序号
    name[node->lens] = '-';
    for (uint32_t i = 0; i < node->nreplicas; i++) {
        // 序号位数会变，digest 按本轮实际长度算
        nlen = 1 + u64tostr(name + node->lens + 1, i, 10);
        MALLOC(item, sizeof(hash_ring_item));
        item->node = node;
        item->digest = _hash_ring_hash(name, node->lens + nlen);
        ring->items[ring->nitems + i] = item;
    }
    if (heap) {
        FREE(name);
    }
    ring->nitems += node->nreplicas;// 新副本先追加在尾部，排序由调用方做(hash_ring_add 归并 / hash_ring_sort)
}
// 在节点链表中按名称查找，返回链表包装（remove 要靠它拿 lnode），不存在返 NULL。
// add 判重复注册与 remove 定位目标必须用同一个谓词：分叉就是加得进去删不掉，node
// 连它全部虚拟副本一起永久留在 items 里
static hash_ring_list *_hash_ring_find(hash_ring_ctx *ring, void *name, size_t lens) {
    hash_ring_list *cur;
    list_foreach(&ring->nodes, ln) {
        cur = UPCAST(ln, hash_ring_list, lnode);
        if (cur->node->lens == lens
            && 0 == memcmp(cur->node->name, name, lens)) {
            return cur;
        }
    }
    return NULL;
}
void hash_ring_sort(hash_ring_ctx *ring) {
    qsort((void **)ring->items, ring->nitems, sizeof(hash_ring_item *), _hash_ring_sort);
}
// 容量判定放在这里而不是 _hash_ring_add_items：那里已经 MALLOC 过 node 并挂进链表,再失败就得回滚。
// 两道顺序不能颠倒——先挡住元素总数溢出 uint32,才能保证下面那个加法在 32 位 size_t 下不回绕;
// 第二道判的是字节数,32 位下 sizeof(指针) * 元素数 会悄悄回绕,4 * 2^30 正好是 0,
// 而 _realloc(ptr, 0) 按契约是释放并返回 NULL,循环随即往 NULL 上写
int32_t hash_ring_add_nosort(hash_ring_ctx *ring, void *name, size_t lens, uint32_t nreplicas) {
    if (NULL == ring
        || NULL == name
        || 0 == lens
        || 0 == nreplicas
        || nreplicas > MAX_REPLICAS) {
        return ERR_FAILED;
    }
    if (ring->nitems > UINT32_MAX - nreplicas
        || (size_t)ring->nitems + nreplicas > SIZE_MAX / sizeof(hash_ring_item *)) {
        return ERR_FAILED;
    }
    if (NULL != _hash_ring_find(ring, name, lens)) {
        return ERR_FAILED;
    }
    hash_ring_node *node;
    MALLOC(node, sizeof(hash_ring_node));
    MALLOC(node->name, lens);
    memcpy(node->name, name, lens);
    node->lens = lens;
    node->nreplicas = nreplicas;
    hash_ring_list *cur;
    MALLOC(cur, sizeof(hash_ring_list));
    cur->node = node;
    list_push_head(&ring->nodes, &cur->lnode);
    ring->nnodes++;
    _hash_ring_add_items(ring, node);
    return ERR_OK;
}
// 只排新加的副本，再与已有的有序段从尾部归并：逐个加节点时不必每次整体重排。
// 新段先拷到临时数组——它就躺在 items 尾部，从尾归并会先写到那里
int32_t hash_ring_add(hash_ring_ctx *ring, void *name, size_t lens, uint32_t nreplicas) {
    uint32_t old = NULL == ring ? 0 : ring->nitems;
    int32_t rtn = hash_ring_add_nosort(ring, name, lens, nreplicas);
    if (ERR_OK != rtn) {
        return rtn;
    }
    uint32_t nnew = ring->nitems - old;
    int64_t i = (int64_t)old - 1, j = (int64_t)nnew - 1, k = (int64_t)ring->nitems - 1;
    hash_ring_item **tmp;
    qsort((void **)(ring->items + old), nnew, sizeof(hash_ring_item *), _hash_ring_sort);
    MALLOC(tmp, sizeof(hash_ring_item *) * nnew);
    memcpy(tmp, ring->items + old, sizeof(hash_ring_item *) * nnew);
    while (j >= 0) {
        if (i >= 0
            && ring->items[i]->digest > tmp[j]->digest) {
            ring->items[k--] = ring->items[i--];
        } else {
            ring->items[k--] = tmp[j--];
        }
    }
    FREE(tmp);
    return rtn;
}
void hash_ring_remove(hash_ring_ctx *ring, void *name, size_t lens) {
    if (NULL == ring
        || NULL == name
        || 0 == lens) {
        return;
    }
    hash_ring_list *cur = _hash_ring_find(ring, name, lens);
    if (NULL == cur) {
        return;
    }
    list_remove(&ring->nodes, &cur->lnode);
    FREE(cur->node->name);
    // items 已有序，用单次 O(n) 原地压缩摘掉该节点的全部副本，顺序不变
    uint32_t write = 0;
    for (uint32_t i = 0; i < ring->nitems; i++) {
        if (ring->items[i]->node == cur->node) {
            FREE(ring->items[i]);
        } else {
            ring->items[write++] = ring->items[i];
        }
    }
    ring->nitems = write;
    FREE(cur->node);
    FREE(cur);
    ring->nnodes--;
}
// 二分查找大于 digest 的第一个节点；超出末尾则环绕返回第一个节点
static hash_ring_item *_hash_ring_find_next_highest_item(hash_ring_ctx *ring, uint64_t digest) {
    if (0 == ring->nitems) {
        return NULL;
    }
    int32_t min = 0;
    int32_t max = (int32_t)ring->nitems - 1;
    int32_t midpointindex;
    hash_ring_item *item = NULL;
    while (1) {
        if (min > max) {
            if (min == (int32_t)ring->nitems) {
                // 超出环末尾，环绕返回第一个节点
                return ring->items[0];
            } else {
                // 返回下一个最大哈希值对应的节点
                return ring->items[min];
            }
        }
        midpointindex = min + (max - min) / 2;
        item = ring->items[midpointindex];
        if (item->digest > digest) {
            // key 在左半区间
            max = midpointindex - 1;// int32_t 可降至 -1，不再下溢
        } else {
            // key 在右半区间
            min = midpointindex + 1;
        }
    }
    return NULL;
}
hash_ring_node *hash_ring_find(hash_ring_ctx *ring, void *key, size_t lens) {
    if (NULL == ring
        || NULL == key
        || 0 == lens) {
        return NULL;
    }
    uint64_t digest = _hash_ring_hash(key, lens);
    hash_ring_item *item = _hash_ring_find_next_highest_item(ring, digest);
    if (item == NULL) {
        return NULL;
    } else {
        return item->node;
    }
}
void hash_ring_print(hash_ring_ctx *ring) {
    uint32_t x;
    printf("----------------------------------------\n");
    printf("hash_ring\n\n");
    printf("Nodes: \n\n");
    hash_ring_list *cur;
    x = 0;
    uint8_t *name;
    hash_ring_item *item;
    list_foreach(&ring->nodes, ln) {
        cur = UPCAST(ln, hash_ring_list, lnode);
        printf("%u: ", x);
        name = cur->node->name;
        fwrite(name, 1, cur->node->lens, stdout);
        printf("\n");
        x++;
    }
    printf("\n");
    printf("Items (%u): \n\n", ring->nitems);
    for (x = 0; x < ring->nitems; x++) {
        item = ring->items[x];
        printf("%" PRIu64 " : ", item->digest);
        name = item->node->name;
        fwrite(name, 1, item->node->lens, stdout);
        printf("\n");
    }
    printf("\n");
    printf("----------------------------------------\n");
}
