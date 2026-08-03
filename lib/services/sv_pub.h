#ifndef SV_PUB_H_
#define SV_PUB_H_

#include "srey/task.h"

// datacenter / subcenter 这类"单实例服务 + 一堆客户端 helper"共用的调用骨架。
// 三个函数都遵循同一条 buf 所有权约定：
//   buf 一律由本函数接管——投递成功时随 copy=0 转移给消息系统，grab 不到目标时由本函数 FREE。
//   调用方组好包就撒手，不必也不能再碰它（buf 传 NULL 表示无载荷，直接透传）。
// 这条约定是有来由的：原先各 helper 是"先 grab 后组包"，靠语句顺序保证 grab 失败时还没组包，
// 骨架抽出来后组包必然提前到调用点，所以释放责任跟着挪进来。

// 协程版请求，只关心错误码；必须在协程中调用。返回目标给的错误码，目标不可达返 ERR_FAILED
int32_t _svpub_call(task_ctx *task, name_t name, subtype_t req, void *buf, size_t lens);
// 协程版请求，要取响应数据；必须在协程中调用。
// 返回值生命周期同 coro_request（本协程下次 yield 前有效）；失败返 NULL 并把 size 置 0。
// size 可传 NULL（不写），erro 不可以——它同 coro_request 是必填出参，传 NULL 即解引用空指针
void *_svpub_call_resp(task_ctx *task, name_t name, subtype_t req, void *buf, size_t lens,
                       size_t *size, int32_t *erro);
// 非协程版请求，不挂起；响应由调用方在 on_responsed 里按 sess 自行配对。
// sess 传 0 表示不要响应，退化为 task_call 的 fire-and-forget（datacenter 的 set/del 用到）；
// 不接受这种用法的服务应在自己的入口先把 0 == sess 拒掉（subcenter 即如此）
int32_t _svpub_send(task_ctx *task, name_t name, subtype_t req, uint64_t sess,
                    void *buf, size_t lens);

// 上面三个按 name 找目标，必然是"组包在前、grab 在后"：目标不在时那份载荷白拷一整遍。
// 载荷可能很大的路径（publish / set）改用下面两个——调用方自己 grab 成功之后再组包，
// 目标不在就一个字节都不拷。dst 由调用方 grab，本函数负责 ungrab；buf 所有权约定同上。
// 载荷只有几十字节的路径（订阅、取键这些）不必换，多一次小拷贝换不回两行 grab 样板
int32_t _svpub_call_dst(task_ctx *dst, task_ctx *task, subtype_t req, void *buf, size_t lens);
int32_t _svpub_send_dst(task_ctx *dst, task_ctx *task, subtype_t req, uint64_t sess,
                        void *buf, size_t lens);

#endif//SV_PUB_H_
