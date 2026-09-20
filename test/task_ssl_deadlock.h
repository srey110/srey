#ifndef TASK_SSL_DEADLOCK_H_
#define TASK_SSL_DEADLOCK_H_

#include "lib.h"

// STATUS_WPEND_SSL 的停读是否会让两端成环:同一进程内 SSL 环回互连,两端各推一大坨,
// 谁都不主动读——读由事件层自己做,而挂起写期间事件层恰恰不读。
// 成环则两侧收到的字节数都停住且没有 close 回调;不成环则两侧都收全并正常关闭。
// 两端都不主动读时环必成,看门狗是唯一出口,故"被 ABORT 收割"与"两端收全"都算通过;
// 真正的失败是停住且无人收割。*ok 只写 1,调用方须预置 0。
// 送的总量必须远大于平台发送缓冲(macOS sendspace 131072),而 buf 个数必须远小于
// MAX_SENDQ_CNT(config.h,默认 1024)——否则会先因队列超限断连,用例就因为错误的理由通过了
void task_ssl_deadlock_start(loader_ctx *loader, const char *name, uint16_t port,
    void *evssl, int32_t *ok);

#endif//TASK_SSL_DEADLOCK_H_
