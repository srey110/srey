#ifndef TASK_ACPSTORM_H_
#define TASK_ACPSTORM_H_

#include "lib.h"

// 拆除期撞在途 accept 的回归测试：
//   每轮起一个独立 ev_ctx 监听，灌一批不等完成的 connect，随即 ev_free。
//   目标是 listen_churn 进不去的那个窗口 —— 它压的是运行期 listen/unlisten 抖动，
//   而这里压的是 ev_free 拆除路径撞上在途 AcceptEx（IOCP 侧 nlsn 完成驱动释放、
//   Unix 侧 listener 摘 changes 后回收）。
// 全部循环跑完置 *ok 为 1；中途 ev_init/ev_listen 失败即 LOG_ERROR 返回。
void task_acpstorm_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok);

#endif//TASK_ACPSTORM_H_
