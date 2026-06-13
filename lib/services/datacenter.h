#ifndef DATACENTER_H_
#define DATACENTER_H_

#include "srey/coro.h"

// keybuf 栈缓冲容量(字节,含 NUL 终止);有效 key 长度须 < DC_KEY_MAX(即 ≤511),客户端编码前/服务端据此拒绝超长 key
#define DC_KEY_MAX 512

/// <summary>
/// 注册 DataCenter task service。
/// 在 loader_init 之后、业务 task 启动之前调用一次,name 由 config.json 的 dc_name 字段决定,默认 "datacenter",空串表示不启动。
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="name">字符串任务名;NULL 或空串时本函数立即返回 ERR_OK 不注册 task</param>
/// <returns>ERR_OK 成功(含跳过的情况);ERR_FAILED 注册失败</returns>
int32_t dc_start(loader_ctx *loader, const char *name);
/// <summary>
/// 写入或覆盖 KV;唤醒所有该 key 的 waiter。必须在协程中调用。
/// </summary>
/// <param name="task">当前 task</param>
/// <param name="dc_name">DataCenter task name(与 dc_start 注册的 name 一致)</param>
/// <param name="key">key 字符串(非 NULL/非空)</param>
/// <param name="val">value 数据(允许 NULL,等价"清空"语义)</param>
/// <param name="size">value 字节数(val=NULL 时传 0,上限 UINT32_MAX)</param>
/// <returns>ERR_OK 成功;ERR_FAILED datacenter 未注册或请求超时</returns>
int32_t coro_dc_set(task_ctx *task, name_t dc_name, const char *key, void *val, size_t size);
/// <summary>
/// 读 KV;key 不存在视为失败(返 NULL + erro=ERR_FAILED),存在(含空值)返 ERR_OK。必须在协程中调用。
/// </summary>
/// <param name="task">当前 task</param>
/// <param name="dc_name">DataCenter task name</param>
/// <param name="key">key 字符串</param>
/// <param name="size">出参:val 字节数(NULL 时不写)</param>
/// <param name="erro">出参:ERR_OK key 存在(含空值);ERR_FAILED key 不存在/非法/datacenter 不可达/超时</param>
/// <returns>val 指针,下次 yield 前有效,框架自动 FREE;返回 NULL 时由 erro 区分:ERR_OK=存在但空值,ERR_FAILED=不存在/失败</returns>
void *coro_dc_get(task_ctx *task, name_t dc_name, const char *key,
                  size_t *size, int32_t *erro);
/// <summary>
/// 读 KV;key 不存在则挂起协程直到 set 触发,或全局 request_timeout 超时。
/// 必须在协程中调用。
/// </summary>
/// <param name="task">当前 task</param>
/// <param name="dc_name">DataCenter task name</param>
/// <param name="key">key 字符串</param>
/// <param name="size">出参:val 字节数</param>
/// <param name="erro">出参:ERR_OK 成功(命中,含空值);ERR_FAILED key 非法/datacenter 不可达/超时</param>
/// <returns>val 指针,下次 yield 前有效;返回 NULL 时由 erro 区分:ERR_OK=命中空值,ERR_FAILED=超时/失败</returns>
void *coro_dc_wait(task_ctx *task, name_t dc_name, const char *key,
                   size_t *size, int32_t *erro);
/// <summary>
/// 删除指定 key 的 KV 条目;只清 _kv,不影响 _pending(仍在等的 waiter 继续等)。
/// 必须在协程中调用。
/// </summary>
/// <param name="task">当前 task</param>
/// <param name="dc_name">DataCenter task name</param>
/// <param name="key">key 字符串</param>
/// <returns>ERR_OK 成功(key 不存在也返 OK);ERR_FAILED key 非法/datacenter 不可达/超时</returns>
int32_t coro_dc_del(task_ctx *task, name_t dc_name, const char *key);
/// <summary>
/// 列出全部 key。调试用,生产 key 量大时谨慎调。
/// 必须在协程中调用。
/// </summary>
/// <param name="task">当前 task</param>
/// <param name="dc_name">DataCenter task name</param>
/// <param name="size">出参:返回 buffer 字节数</param>
/// <param name="erro">出参:ERR_OK 成功(含空,返 NULL+size 0);ERR_FAILED datacenter 不可达/超时</param>
/// <returns>key 列表 buffer,每条格式 | u16 klen(大端) | key |;空时 size=0 返回 NULL(erro=ERR_OK),失败返 NULL 且 erro=ERR_FAILED;指针下次 yield 前有效</returns>
void *coro_dc_keys(task_ctx *task, name_t dc_name,
                   size_t *size, int32_t *erro);
/// <summary>
/// 写入或覆盖 KV;不挂起,可在非协程上下文调用。sess=0 fire-and-forget;sess!=0 业务在 _response 收 OK 确认。
/// </summary>
/// <param name="task">当前 task(作为 sess!=0 时的 response 目标)</param>
/// <param name="dc_name">DataCenter task name</param>
/// <param name="sess">会话 id;0=fire-and-forget,非 0=业务自管响应配对</param>
/// <param name="key">key 字符串(非 NULL/非空)</param>
/// <param name="val">value 数据(允许 NULL,等价软清空)</param>
/// <param name="size">value 字节数(上限 UINT32_MAX)</param>
/// <returns>ERR_OK 成功投递;ERR_FAILED datacenter 不可达</returns>
int32_t dc_set(task_ctx *task, name_t dc_name, uint64_t sess, const char *key, void *val, size_t size);
/// <summary>
/// 删除 key;不挂起。sess=0 fire-and-forget;sess!=0 业务在 _response 收 OK 确认。
/// 只清 _kv,不影响 _pending(已挂起 waiter 继续等)。
/// </summary>
/// <param name="task">当前 task(sess!=0 时的 response 目标)</param>
/// <param name="dc_name">DataCenter task name</param>
/// <param name="sess">会话 id;0=fire-and-forget,非 0=业务自管响应配对</param>
/// <param name="key">key 字符串(非 NULL/非空)</param>
/// <returns>ERR_OK 成功投递;ERR_FAILED datacenter 不可达</returns>
int32_t dc_del(task_ctx *task, name_t dc_name, uint64_t sess, const char *key);
/// <summary>
/// 读 KV;不挂起,sess 必须非 0(读需响应通道,sess=0 返 ERR_FAILED);业务在 _response 收 val(key 不存在时 erro=ERR_FAILED)。
/// </summary>
/// <param name="task">当前 task(response 目标)</param>
/// <param name="dc_name">DataCenter task name</param>
/// <param name="sess">会话 id;必须非 0(读操作需响应配对,0 返 ERR_FAILED)</param>
/// <param name="key">key 字符串(非 NULL/非空)</param>
/// <returns>ERR_OK 成功投递;ERR_FAILED key 非法/sess=0/datacenter 不可达</returns>
int32_t dc_get(task_ctx *task, name_t dc_name, uint64_t sess, const char *key);
/// <summary>
/// 读 KV;不命中时 DataCenter 仍挂 pending,响应到达时机由 set 触发(业务自管超时,framework 不提供
/// 自动唤醒,因非协程版 sess 不在 coro_sess 表中)。挂起的 waiter 超过本 task 的 request_timeout 后即视为
/// 过期:此后的 set 不再唤醒它,并由 DataCenter 回收;需更久请调高本 task 的 request_timeout。
/// sess 必须非 0(读需响应通道,sess=0 返 ERR_FAILED);业务在 _response 收 val。
/// </summary>
/// <param name="task">当前 task(response 目标)</param>
/// <param name="dc_name">DataCenter task name</param>
/// <param name="sess">会话 id;必须非 0(读操作需响应配对,0 返 ERR_FAILED)</param>
/// <param name="key">key 字符串(非 NULL/非空)</param>
/// <returns>ERR_OK 成功投递;ERR_FAILED key 非法/sess=0/datacenter 不可达</returns>
int32_t dc_wait(task_ctx *task, name_t dc_name, uint64_t sess, const char *key);
/// <summary>
/// 列出全部 key,每条格式 | u16 klen(大端) | key |;不挂起,sess 必须非 0(读需响应通道,sess=0 返 ERR_FAILED);业务在 _response 收 buffer。
/// </summary>
/// <param name="task">当前 task(response 目标)</param>
/// <param name="dc_name">DataCenter task name</param>
/// <param name="sess">会话 id;必须非 0(读操作需响应配对,0 返 ERR_FAILED)</param>
/// <returns>ERR_OK 成功投递;ERR_FAILED sess=0/datacenter 不可达</returns>
int32_t dc_keys(task_ctx *task, name_t dc_name, uint64_t sess);

#endif//DATACENTER_H_
