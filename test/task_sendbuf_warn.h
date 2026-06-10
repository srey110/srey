#ifndef TASK_SENDBUF_WARN_H_
#define TASK_SENDBUF_WARN_H_

#include "lib.h"

// wb_size 字节告警 + 数据完整性回归测试。
// 同 task 内：server listen + client coro_connect，client 一次性 ev_send 大块数据
// 触发 _uev_add_write_inloop 累加 → buf_s 字节数 >= WB_WARN_INIT_SIZE 触发 LOG_WARN
// （"send buf growing"），全部数据应完整到达 server，过程不崩、无 ASan 报警。
// 通过条件：server 端累计字节 == ROUNDS * BYTES_PER_ROUND，*ok=1。
void task_sendbuf_warn_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok);

#endif//TASK_SENDBUF_WARN_H_
