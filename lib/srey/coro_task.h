#ifndef CORO_TASK_H_
#define CORO_TASK_H_

#include "srey/task.h"
#include "coro/coro.h"

// sess 唤醒约定:客户端 connect 默认 setsess=1(ud.sess=skid),coro_ssl_exchange/coro_handshaked/coro_send/coro_slice
// 等挂起等 skid 消息的 API 会被自动唤醒;服务端 accept 连接 ud.sess=0,须显式 coro_sync 设置,否则这些等待挂到超时。
// UDP coro_sendto 同此约定:ud.sess 不会自动清零,须在首次调用 coro_sendto 前显式 coro_sync 一次,
// 之后该 skid 上持续有效,可连续/并发多次调用 coro_sendto,无需每次重新同步。
// 换线程约定:协程挂起后可能在另一个 worker 线程上恢复。协程代码里的线程局部变量一律经 TLS_DEFINE
// 的访问器取,用法限制见 lib/base/macro_util.h;也不许跨挂起持有 rwlock_distr / 互斥锁(恢复后解锁的是另一个线程)

/// <summary>
/// 设定协程 task 的协程栈大小，启动期单线程调用一次，之后注册的协程 task 都用它
/// </summary>
/// <param name="stack_size">协程栈大小（字节）。0 取下界；非 0 但越界时打一行 WARN 并夹到对应
/// 边界(见 coro_stack_fit)，故须在日志开起来之后调</param>
void coro_task_stack(size_t stack_size);
/// <summary>
/// 注册协程任务
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="name">字符串任务名；NULL 或空串表示匿名</param>
/// <param name="quecap">消息队列容量（条数）；0 用默认 TASK_QUEUE_CAP</param>
/// <param name="_startup">任务初始化回调函数</param>
/// <param name="_closing">任务关闭回调函数,做业务相关收尾工作._closing执行后，不代表该任务已经无引用</param>
/// <param name="_argfree">用户参数释放函数</param>
/// <param name="arg">用户参数</param>
/// <returns>task_ctx，失败返回 NULL</returns>
task_ctx *coro_task_register(loader_ctx *loader, const char *name, uint32_t quecap,
                             _task_startup_cb _startup, _task_closing_cb _closing,
                             free_cb _argfree, void *arg);
/// <summary>
/// 获取coro_task_register时传入的用户参数
/// </summary>
/// <param name="task">task_ctx</param>
/// <returns>用户参数</returns>
void *coro_get_arg(task_ctx *task);
/// <summary>
/// 取协程 task 的调度器，fork / serial / dump 直接用 coro.h 的接口
/// </summary>
/// <param name="task">task_ctx</param>
/// <returns>调度器，owner 是 task 本身；task 不是协程 task(coro_task_register 建的以外)返回 NULL</returns>
coro_ctx *coro_task_co(task_ctx *task);
/// <summary>
/// 当前是否正跑在本 task 的协程内。coro_send / coro_close / coro_sleep 这些会挂起的接口只能
/// 在协程内调，非协程调用会撞断言；调用路径不确定时(如析构里收尾)先问一次再决定发不发
/// </summary>
/// <param name="task">task_ctx</param>
/// <returns>1 在协程内；0 不在，或该 task 不是协程类型</returns>
int32_t coro_incoro(task_ctx *task);
/// <summary>
/// 将 UDP socket 的 session 与 skid 绑定，使收到的数据包能路由到当前协程
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="sk">连接标识</param>
/// <returns>ERR_OK 仅表示命令已入队（fd 非 INVALID_SOCK），不代表绑定已生效；
///   绑定必然先于随后同一 fd 的 sendto 落定，两者投给同一 watcher 的同一条命令队列，FIFO</returns>
int32_t coro_sync(task_ctx *task, sock_ctx *sk);
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
/// <param name="erro">错误码；必须非 NULL，函数内裸解引用</param>
/// <param name="lens">返回数据长度；可传 NULL; 只在返回非 NULL 时写入</param>
/// <returns>响应数据；仅在当前协程下次 yield（再调任意 coro_* API）前有效，
///   下次 resume 时框架自动释放，需要保留请自行拷贝</returns>
void *coro_request(task_ctx *dst, task_ctx *src,
                   subtype_t rtype, void *data, size_t size, int32_t copy,
                   int32_t *erro, size_t *lens);
/// <summary>
/// 切换为SSL链接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="client">1 作为客户端 0 作为服务端</param>
/// <param name="evssl">evssl_ctx</param>
/// <returns>ERR_OK 成功；fd 为 INVALID_SOCK 时不挂起,直接按失败返回(同 coro_recv)</returns>
int32_t coro_ssl_exchange(task_ctx *task, sock_ctx *sk, int32_t client, struct evssl_ctx *evssl);
/// <summary>
/// 等待握手完成
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="err">错误码；必须非 NULL，函数内裸解引用</param>
/// <param name="size">返回数据长度；可传 NULL; 只在返回非 NULL 时写入</param>
/// <returns>握手数据；仅在当前协程下次 yield（再调任意 coro_* API）前有效，
///   下次 resume 时框架自动释放，需要保留请自行拷贝。
///   fd 为 INVALID_SOCK 时不挂起,直接返回 NULL(同 coro_recv)</returns>
void *coro_handshaked(task_ctx *task, sock_ctx *sk, int32_t *err, size_t *size);
/// <summary>
/// 等待 task_connect 已发起的 CONNECT 完成；超时由本函数关闭 fd，其余失败时 fd 已由事件/协议层关闭；
/// evssl 非 NULL 时紧接着等 SSL 握手完成
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="evssl">非 NULL 时额外等待 SSL 握手；须是 connect 时已同步触发握手的 evssl，协议层后续才发起握手的场景不适用</param>
/// <returns>ERR_OK 成功；fd 为 INVALID_SOCK 时不挂起,直接按失败返回(同 coro_recv)</returns>
int32_t coro_wait_connect(task_ctx *task, sock_ctx *sk, struct evssl_ctx *evssl);
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
/// <param name="sk">连接标识</param>
/// <returns>ERR_OK 成功</returns>
int32_t coro_connect(task_ctx *task, pack_type pktype,
                     struct evssl_ctx *evssl, const char *ip, uint16_t port,
                     int32_t netev, void *extra,
                     sock_ctx *sk);
/// <summary>
/// 同步关闭连接：发起关闭后挂起协程等 CLOSE 消息，确保协议层 close 回调执行完毕再返回。
/// 重连前调用,避免旧连接异步 teardown 与新连接共享 ctx 时清掉新 fd;须在协程内调用,连接已失效时调用方自行跳过。
/// 未发数据的丢弃契约同 ev_close：等的是"关完了",不是"发完了"。
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="sk">连接标识</param>
void coro_close(task_ctx *task, sock_ctx *sk);
/// <summary>
/// TCP发送
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="data">数据</param>
/// <param name="len">数据长度</param>
/// <param name="size">返回数据长度；可传 NULL; 只在返回非 NULL 时写入</param>
/// <param name="copy">1 拷贝数据 0 不拷贝</param>
/// <returns>响应数据；仅在当前协程下次 yield（再调任意 coro_* API）前有效，
///   下次 resume 时框架自动释放，需要保留请自行拷贝。
///   发送失败 / 超时 / 连接已关返回 NULL</returns>
void *coro_send(task_ctx *task, sock_ctx *sk, void *data, size_t len, size_t *size, int32_t copy);
/// <summary>
/// 同步接收下一个响应包(不发送):用于一次请求产生多个响应的场景,如 MySQL 多结果集续读
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="size">输出:数据长度；可传 NULL; 只在返回非 NULL 时写入</param>
/// <returns>响应数据指针,仅在本协程下次挂起前有效(同 coro_send);超时/断开返回 NULL。
///   fd 为 INVALID_SOCK 时不挂起,直接返回 NULL——连接已 teardown 时挂上去等不到唤醒</returns>
void *coro_recv(task_ctx *task, sock_ctx *sk, size_t *size);
/// <summary>
/// 等待分片消息。等待按 skid 排队、只认队头，而本函数每次调用都重新排到队尾——同一连接上
/// 有其他等待者时，后续分片会被派给别人，两边各拿到半截。整趟分片循环期间调用方须保证
/// 这条连接上没有并发的等待者
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="size">数据长度；可传 NULL; 只在返回非 NULL 时写入</param>
/// <param name="end">1 分片结束 0未结束；必须非 NULL，函数内裸解引用；
///   返回 NULL 时保证已写 0</param>
/// <returns>分片数据；仅在当前协程下次 yield（再调任意 coro_* API）前有效，
///   下次 resume 时框架自动释放，需要保留请自行拷贝</returns>
void *coro_slice(task_ctx *task, sock_ctx *sk, size_t *size, int32_t *end);
/// <summary>
/// UDP发送并等待响应；调用前须显式 coro_sync 一次（同 TCP 服务端 accept 连接的约定，见文件头注释）
/// 之后同一 skid 上可连续/并发多次调用；并发时多次调用与多次响应按到达顺序 FIFO 配对，
/// 网络乱序时配对结果仍可能与发送顺序不一致——UDP 协议本身无法避免的限制
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="data">数据；copy=0 时所有权转移给框架，调用方不得再 FREE</param>
/// <param name="len">数据长度</param>
/// <param name="size">返回数据长度；可传 NULL; 只在返回非 NULL 时写入</param>
/// <param name="copy">1 拷贝数据 0 不拷贝(转移所有权)</param>
/// <returns>响应数据（已去除 netaddr_ctx 前缀）；仅在当前协程下次 yield（再调任意 coro_* API）前有效，
///   下次 resume 时框架自动释放，需要保留请自行拷贝。
///   发送失败 / 超时 / 连接已关返回 NULL</returns>
void *coro_sendto(task_ctx *task, sock_ctx *sk,
                  const char *ip, const uint16_t port,
                  void *data, size_t len, size_t *size, int32_t copy);
message_ctx *_coro_wait(task_ctx *task, uint64_t sess, msg_type mtype, uint32_t ms);

#endif//CORO_TASK_H_
