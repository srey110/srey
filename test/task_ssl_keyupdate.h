#ifndef TASK_SSL_KEYUPDATE_H_
#define TASK_SSL_KEYUPDATE_H_

#include "lib.h"

// 单向大流量下挂起写(STATUS_WPEND_SSL)那条路能不能自己走完:置位 → 排空 → 补读 → 收全。
// 与 task_ssl_deadlock 分工:那个负载对称、环必成，钉的是看门狗兜底(被 ABORT 收割也算过)；
// 这个负载单向、结构上不成环，钉的是正常推进。
// 它**够不到** KEYUPDATE_READ:实测该位置位 0 次——那一档要对端把 post-handshake 消息
// 拆到多条 TLS 记录，KeyUpdate 本身触发不了(理由见 evpub.h)。别拿它当方向 B 的回归网，
// 那一档的前提由 test_event.c 的 test_ssl_write_wants_read 钉着。
// 负载是单向的:A 只推不收，B 只收不推，只在收满 KU_STEP 时排一次 KeyUpdate 并回 1 字节把它带出去。
// B 的出流量全生命周期有硬上界(BLK_CNT*BLK_BYTES/KU_STEP 次 x 约 50 字节)，远小于任何平台的
// 最小发送缓冲，故 B 的 SSL_write 不可能 WANT_WRITE、B 从不进 WPEND_SSL、B 从不停读——
// A 的挂起写必然能排空，结构上不成环。这条不变式是本用例的承重墙:
// 谁要加大 ack 或提高 KeyUpdate 频率，先重算这个上界。
// 判据:DEADLINE 内 B 收全 8MB、A 收齐全部 ack，两边收到的内容都没坏，且 A 真读到过 KeyUpdate——
// ev_keyupdate 只投递、排程失败只落 WARN，投递次数证明不了排上了，故在 evssl 的 SSL_CTX 上挂 msg 回调数 A 读到的条数。
// 任何 close 回调或停住都只可能是接力坏了。
// *ok 只写 1，调用方须预置 0。evssl 必须是本用例专用的 TLS1.3 上下文(会被挂上 msg 回调)，
// 协商不到 1.3 时一次 KeyUpdate 都排不上，用例报失败
void task_ssl_keyupdate_start(loader_ctx *loader, const char *name, uint16_t port,
    void *evssl, int32_t *ok);

#endif//TASK_SSL_KEYUPDATE_H_
