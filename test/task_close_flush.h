#ifndef TASK_CLOSE_FLUSH_H_
#define TASK_CLOSE_FLUSH_H_

#include "lib.h"

// ev_close 的"发完即关、关闭有界"契约回归测试,三段:
// 1) 小包:每轮 coro_connect → ev_send(SMALL) → ev_close,server 端累计应精确等于发送总量,
//    close_cb 触发 ROUNDS 次。ev_send 只把 4KB 入队、等轮末统一发,所以送达靠两条路之一:
//    send 与 close 落在同一批命令里(常态)时靠关闭前那次冲刷(_evpub_close_flush_tcp),
//    分在两批时靠上一轮派发末尾的统一冲刷。IOCP 上 ev_send 只入队投探针,探针回来之前就关的靠关闭冲刷
// 2) 大包:一轮 ev_send(BIG) → ev_close,不断言送达量(写不进内核的按契约丢弃),
//    只断言 close_cb 照样触发——即关闭有界,不会因对端来不及读而滞留
// 3) 解析出错:port+5 起一个 PACK_HTTP 监听,发超长且无 CRLFCRLF 的头,server 端在收包回调里
//    就地关连接,断言它的 erro 是 LOCAL
// 三段都通过则 *ok=1。
// 冲刷本身的各分支(队列空早退、KEYUPDATE_WRITE 整队不冲、握手期照冲)由 test_event.c 直接单测
void task_close_flush_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok);

#endif//TASK_CLOSE_FLUSH_H_
