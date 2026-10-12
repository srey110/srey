#include "utils/idgen.h"

#define _ID_BLOCK 1024 // createid 每个线程一次领走的号数
// createid 每个线程手里的一段号：[next, end)
typedef struct id_seg {
    uint64_t next;//下一个要发的号
    uint64_t end;//这段的末尾(不含)
}id_seg;
static uint16_t _serviceid = 1;
static atomic64_t _ids = 1;//发号计数，各线程按 _ID_BLOCK 一段一段领
TLS_DEFINE(id_seg, _idseg, 1)//本线程手里那段号

int32_t serviceid(uint16_t id) {
    if (id > SERVICEID_MAX) {
        return ERR_FAILED;
    }
    _serviceid = id;
    return ERR_OK;
}
// 每个线程一次领一段号，段内自己发，全局计数每 _ID_BLOCK 次才碰一次。
// 段与段不重叠所以仍全局唯一；同一线程内递增，跨线程不保证先后
uint64_t createid(void) {
    id_seg *seg = _idseg_tls();
    if (seg->next == seg->end) {
        seg->next = (uint64_t)ATOMIC64_ADD_RELAXED(&_ids, _ID_BLOCK);
        seg->end = seg->next + _ID_BLOCK;
    }
    return ((uint64_t)_serviceid << 48) | (seg->next++ & 0xFFFFFFFFFFFFULL);
}
