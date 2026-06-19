#ifndef CORO_TASK_H_
#define CORO_TASK_H_

#include "srey/task.h"

typedef struct coro_serial_ctx coro_serial_ctx;

/// <summary>
/// 初始化协程描述符，设置协程栈大小
/// </summary>
/// <param name="stack_size">协程栈大小（字节），0 使用默认值</param>
void coro_desc_init(size_t stack_size);
/// <summary>
/// 注册协程任务
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="name">字符串任务名；NULL 或空串表示匿名</param>
/// <param name="quecap">消息队列容量；0 用默认 ONEK</param>
/// <param name="_startup">任务初始化回调函数</param>
/// <param name="_closing">任务关闭回调函数,做业务相关收尾工作._closing执行后，不代表该任务已经无引用</param>
/// <param name="_argfree">用户参数释放函数</param>
/// <param name="arg">用户参数</param>
/// <returns>task_ctx，失败返回 NULL</returns>
task_ctx *coro_task_register(loader_ctx *loader, const char *name, size_t quecap,
                             _task_startup_cb _startup, _task_closing_cb _closing,
                             free_cb _argfree, void *arg);
/// <summary>
/// 获取coro_task_register时传入的用户参数
/// </summary>
/// <param name="task">task_ctx</param>
/// <returns>用户参数</returns>
void *coro_get_arg(task_ctx *task);
/// <summary>
/// 将 UDP socket 的 session 与 skid 绑定，使收到的数据包能路由到当前协程
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="fd">socket 句柄</param>
/// <param name="skid">链接ID</param>
/// <returns>ERR_OK 成功</returns>
int32_t coro_sync(task_ctx *task, SOCKET fd, uint64_t skid);
/// <summary>
/// 休眠
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="ms">毫秒</param>
void coro_sleep(task_ctx *task, uint32_t ms);
/// <summary>
/// 任务间通信 请求
/// </summary>
/// <param name="dst">目标任务</param>
/// <param name="src">发起者</param>
/// <param name="rtype">请求类型</param>
/// <param name="data">数据</param>
/// <param name="size">数据长度</param>
/// <param name="copy">1 拷贝数据 0 不拷贝数据</param>
/// <param name="erro">错误码</param>
/// <param name="lens">返回数据长度</param>
/// <returns>响应数据；仅在当前协程下次 yield（再调任意 coro_* API）前有效，
///   下次 resume 时框架自动释放，需要保留请自行拷贝</returns>
void *coro_request(task_ctx *dst, task_ctx *src,
                   subtype_t rtype, void *data, size_t size, int32_t copy,
                   int32_t *erro, size_t *lens);
/// <summary>
/// 切换为SSL链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="client">1 作为客户端 0 作为服务端</param>
/// <param name="evssl">evssl_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t coro_ssl_exchange(task_ctx *task, SOCKET fd, uint64_t skid,
                          int32_t client, struct evssl_ctx *evssl);
/// <summary>
/// 等待握手完成
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="err">错误码</param>
/// <param name="size">返回数据长度</param>
/// <returns>握手数据；仅在当前协程下次 yield（再调任意 coro_* API）前有效，
///   下次 resume 时框架自动释放，需要保留请自行拷贝</returns>
void *coro_handshaked(task_ctx *task, SOCKET fd, uint64_t skid, int32_t *err, size_t *size);
/// <summary>
/// 链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="pktype">数据包类型</param>
/// <param name="evssl">evssl_ctx</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="netev">task_netev</param>
/// <param name="extra">ud_cxt extra</param>
/// <param name="fd">SOCKET</param>
/// <param name="skid">链接ID</param>
/// <returns>ERR_OK 成功</returns>
int32_t coro_connect(task_ctx *task, pack_type pktype,
                     struct evssl_ctx *evssl, const char *ip, uint16_t port,
                     int32_t netev, void *extra,
                     SOCKET *fd, uint64_t *skid);
/// <summary>
/// 同步关闭连接：发起关闭后挂起协程等 CLOSE 消息，确保协议层 close 回调执行完毕再返回。
/// 重连前调用,避免旧连接异步 teardown 与新连接共享 ctx 时清掉新 fd;须在协程内调用,连接已失效时调用方自行跳过。
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="immed">0 优雅关闭(send queue 空时退化为立即) 1 立即关闭</param>
void coro_close(task_ctx *task, SOCKET fd, uint64_t skid, int32_t immed);
/// <summary>
/// TCP发送
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="data">数据</param>
/// <param name="len">数据长度</param>
/// <param name="size">返回数据长度</param>
/// <param name="copy">1 拷贝数据 0 不拷贝</param>
/// <returns>响应数据；仅在当前协程下次 yield（再调任意 coro_* API）前有效，
///   下次 resume 时框架自动释放，需要保留请自行拷贝</returns>
void *coro_send(task_ctx *task, SOCKET fd, uint64_t skid,
                void *data, size_t len, size_t *size, int32_t copy);
/// <summary>
/// 等待分片消息
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="size">数据长度</param>
/// <param name="end">1 分片结束 0未结束</param>
/// <returns>分片数据；仅在当前协程下次 yield（再调任意 coro_* API）前有效，
///   下次 resume 时框架自动释放，需要保留请自行拷贝</returns>
void *coro_slice(task_ctx *task, SOCKET fd, uint64_t skid, size_t *size, int32_t *end);
/// <summary>
/// UDP发送
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="data">数据 需要手动Free</param>
/// <param name="len">数据长度</param>
/// <param name="size">返回数据长度</param>
/// <param name="copy">1 不自动释放, 0 自动释放</param>
/// <returns>响应数据（已去除 netaddr_ctx 前缀）；仅在当前协程下次 yield（再调任意 coro_* API）前有效，
///   下次 resume 时框架自动释放，需要保留请自行拷贝</returns>
void *coro_sendto(task_ctx *task, SOCKET fd, uint64_t skid,
                  const char *ip, const uint16_t port,
                  void *data, size_t len, size_t *size, int32_t copy);
/// <summary>
/// 在新协程中执行 func(task, arg)，fire-and-forget；当前协程不让出。
/// 新协程通过 task 自发 MSG_TYPE_FORK 消息调度，不走时间轮；与 REQUEST 同模式：
/// 每条 fork 都新建协程（从 qucopool 取或新建）。
/// arg 由调用方管理生命周期；func 内部 abort/segfault 终止进程（C 无 xpcall 兜底）。
/// </summary>
/// <param name="task">所属 task</param>
/// <param name="func">协程任务函数：func(task, arg)</param>
/// <param name="arg">透传给 func 的 user 数据指针</param>
void coro_fork(task_ctx *task,
               void (*func)(task_ctx *task, void *arg),
               void *arg);
/// <summary>
/// 并发执行 n 个 funcs[i](task, args[i])，等全部完成后返回（barrier 模式）。
/// 调用方必须身处协程内（startup/timeout/on_* 回调内部均满足），否则返回 ERR_FAILED。
/// C 无闭包：每个 funcs[i] 的返回值/错误码须由业务自己写入 args[i] 内的 out 字段。
/// 总耗时 ≈ max(t_i)，而非 sum(t_i)。
/// </summary>
/// <param name="task">所属 task</param>
/// <param name="n">并发任务数；n 小于等于 0 立即返回 ERR_OK</param>
/// <param name="funcs">长度为 n 的函数指针数组</param>
/// <param name="args">长度为 n 的参数指针数组，args[i] 与 funcs[i] 配对</param>
/// <returns>ERR_OK 成功；ERR_FAILED 调用方不在协程内</returns>
int32_t coro_fork_wait(task_ctx *task,
                       int32_t n,
                       void (*funcs[])(task_ctx *task, void *arg),
                       void *args[]);
/// <summary>
/// 创建协程串行化执行器（critical section）。同 task 内多协程对同一资源并发访问时
/// 串行进入，避免穿插；同一协程嵌套调用安全（ref 计数）。
/// </summary>
/// <param name="task">所属 task</param>
/// <returns>coro_serial_ctx；销毁用 coro_serial_free</returns>
coro_serial_ctx *coro_serial_new(task_ctx *task);
/// <summary>
/// 销毁串行化执行器；调用方保证无 in-flight 协程持锁或在等待队列中。
/// </summary>
/// <param name="serial">coro_serial_ctx</param>
void coro_serial_free(coro_serial_ctx *serial);
/// <summary>
/// 进入临界区执行 func(task, arg)。调用方必须身处协程内，否则返回 ERR_FAILED。
/// 同协程嵌套安全（ref 计数）；跨协程时按 FIFO 排队挂起，前一个完成时唤醒下一个。
/// C 无 xpcall：func 内 abort 终止进程；调用方需自行保证 func 不崩。
/// </summary>
/// <param name="serial">coro_serial_ctx</param>
/// <param name="func">临界区回调：func(task, arg)</param>
/// <param name="arg">透传给 func 的参数（生命周期由调用方管理）</param>
/// <returns>ERR_OK 成功；ERR_FAILED 调用方不在协程内</returns>
int32_t coro_serial_call(coro_serial_ctx *serial,
                    void (*func)(task_ctx *task, void *arg),
                    void *arg);
/// <summary>
/// 转储当前 task 所有挂起协程为文本 buffer(调试用)。C 协程无栈回溯,每条仅 sess / mtype / 挂起时长(ms)。
/// 返回 binary 内部 MALLOC 的 buffer,所有权转给调用方,用完 FREE。
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="size">出参:buffer 字节数;NULL 不写</param>
/// <returns>文本 buffer(调用方 FREE);非协程 task(TASK_MCO 以外)返回 NULL 且 size=0</returns>
char *coro_dump(task_ctx *task, size_t *size);

#endif//CORO_TASK_H_
