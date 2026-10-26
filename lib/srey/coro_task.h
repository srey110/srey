#ifndef CORO_TASK_H_
#define CORO_TASK_H_

#include "srey/task.h"
#include "coro/coro.h"

// 连接池等待者的唤醒 tag（等待标记，不是消息类型）：只用于 coro_wait / coro_wake 的队头匹配，
// 取负值避免与 MSG_TYPE 混用；-MSG_TYPE_* 那段小负数已被 coro_task.c 的 CORO_TAG_RECV_ANY 用了，故从 -0x40000000 起。coro_dump 里显示为 DB_POOL
#define CORO_TAG_DB_POOL (-0x40000000)
// 连接池维护协程的定时等待 tag：每秒到期一次是常态，超时不打日志；coro_dump 里显示为 DB_POOL_TICK
#define CORO_TAG_DB_POOL_TICK (-0x40000001)

// sess 唤醒约定:客户端 connect 默认 setsess=1(ud.sess=skid),coro_ssl_exchange/coro_handshaked/coro_send/coro_slice
// 等挂起等 skid 消息的 API 会被自动唤醒;服务端 accept 连接 ud.sess=0,须设好会话键,否则这些等待挂到超时:
// listen 带 NETEV_SESS 则 accept 时就设好;事后 coro_sync 是异步生效的,只适合服务端先发数据的协议(客户端先发的首包会落到回调上)。
// coro_send / coro_recv / coro_slice 只收回复(要过各协议的 may_resume),对端主动发的包(如 MQTT 的 CONNECT)落到回调上,
// 要挂起等这类包用 coro_recv_any。
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
/// 注册协程任务。本 task 里 coro_utils 内部建的连接关闭时，也会投到 task_closed 注册的关闭回调(见 task_closed)
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="name">字符串任务名；NULL 或空串表示匿名</param>
/// <param name="quecap">消息队列容量（条数）；0 用默认 TASK_QUEUE_CAP</param>
/// <param name="_startup">任务初始化回调函数</param>
/// <param name="_closing">任务关闭回调函数,做业务相关收尾工作._closing执行后，不代表该任务已经无引用</param>
/// <param name="_argfree">用户参数释放函数</param>
/// <param name="arg">用户参数</param>
/// <returns>task_ctx；失败(名字重名)返回 NULL，此时 _argfree 非 NULL 就已用它释放了 arg，
///   与 task_register 失败时所有权留在调用方不同</returns>
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
/// 把连接的会话键设为 skid，使收到的消息能唤醒在它上面挂起的协程。
/// UDP 与 listen 没带 NETEV_SESS 的 accept 连接须先调一次(客户端 connect 默认已设)，见文件头约定
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="sk">连接标识</param>
/// <returns>ERR_OK 仅表示命令已入队（fd 非 INVALID_SOCK），不代表绑定已生效；
///   绑定必然先于随后同一 fd 的 sendto 落定，两者投给同一 watcher 的同一条命令队列，FIFO</returns>
int32_t coro_sync(task_ctx *task, sock_ctx *sk);
/// <summary>
/// 把连接转给 handle 那个 task(同 ev_ud_handle)，并放掉本 task 在这条连接上的会话条目(见 coro_unkeep)。
/// 协程 task 转交自己等过的连接要用它：直接调 ev_ud_handle 的话 CLOSE 投给新 task，本 task 那条条目再没有路径能删。
/// 只能在本 task 的上下文里调(消息回调或它的协程里)；转交之后本 task 别再在这条连接上等。
/// 转交是异步生效的(见 ev_ud_handle)，生效前到达本 task 的包没有等待者，照常走注册的回调
/// </summary>
/// <param name="task">本 task，须是协程 task</param>
/// <param name="sk">连接标识</param>
/// <param name="handle">目标 task 句柄</param>
/// <returns>同 ev_ud_handle；失败时会话条目不动</returns>
int32_t coro_bind_task(task_ctx *task, sock_ctx *sk, name_t handle);
/// <summary>
/// 休眠
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="ms">毫秒；0 直接返回、不挂起，也就不会让出给本 task 的其他协程</param>
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
/// <param name="lens">返回数据长度；可传 NULL; 等到响应就写入(无载荷的响应返回 NULL 也写)，超时不写</param>
/// <returns>响应数据；仅在当前协程下次 yield（再调任意 coro_* API）前有效，
///   本协程下次挂起或结束时由框架释放，需要保留请自行拷贝</returns>
void *coro_request(task_ctx *dst, task_ctx *src,
                   subtype_t rtype, void *data, size_t size, int32_t copy,
                   int32_t *erro, size_t *lens);
/// <summary>
/// 切换为SSL链接并等握手完成。明文建的连接要在 coro_connect / task_listen 时带 NETEV_AUTHSSL，握手结果才会投回来，
/// 否则握手成了也等满读超时、连接被关；事件线程拒收这次切换(已是 SSL 连接、未连通、已在关)时同样等满超时后关
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
/// <param name="size">返回数据长度；可传 NULL; 等到握手结果就写入(无数据返回 NULL 也写)，超时或连接已关不写</param>
/// <returns>握手数据；仅在当前协程下次 yield（再调任意 coro_* API）前有效，
///   本协程下次挂起或结束时由框架释放，需要保留请自行拷贝。
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
/// <param name="extra">协议专用附加参数(ud_cxt extra，如 websock 握手的校验 key)；所有权一律交给框架，同步失败也已在函数内释放，之后按 pktype 的 udfree 释放</param>
/// <param name="sk">连接标识</param>
/// <returns>ERR_OK 成功</returns>
int32_t coro_connect(task_ctx *task, pack_type pktype,
                     struct evssl_ctx *evssl, const char *ip, uint16_t port,
                     int32_t netev, void *extra,
                     sock_ctx *sk);
/// <summary>
/// 同步关闭连接：发起关闭后挂起协程等 CLOSE 消息，确保协议层 close 回调执行完毕再返回。
/// 重连前调用,避免旧连接异步 teardown 与新连接共享 ctx 时清掉新 fd;须在协程内调用。
/// 未发数据的丢弃契约同 ev_close：等的是"关完了",不是"发完了"。
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="sk">连接标识</param>
/// <returns>ERR_OK 已收到 CLOSE 或连接本已失效；ERR_FAILED 等满 netread 超时仍没收到 CLOSE（网络线程或本 task 队列积压，超时前没处理到这次关闭），
/// 此时旧连接的 close 回调还会晚到，与它共用 ctx 的新连接不能建</returns>
int32_t coro_close(task_ctx *task, sock_ctx *sk);
/// <summary>
/// TCP发送并等待响应。同一连接上多个协程并发收发时，须用 coro_serial 把整个"发请求→收完响应"圈住：
/// 等待者按连接排队、只认队头，回包按到达顺序交给先挂起的协程，而服务端不保证按请求顺序回包，不圈就会对错号
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="data">数据</param>
/// <param name="len">数据长度</param>
/// <param name="size">返回响应的 size；可传 NULL; 只在返回非 NULL 时写入。
///   口径见 prots_unpack 的 size 契约：结构化协议返回的是 pack 对象，不能拿 size 直接读</param>
/// <param name="copy">1 拷贝数据 0 不拷贝</param>
/// <returns>响应数据；仅在当前协程下次 yield（再调任意 coro_* API）前有效，
///   本协程下次挂起或结束时由框架释放，需要保留请自行拷贝。
///   发送失败 / 超时 / 连接已关返回 NULL</returns>
void *coro_send(task_ctx *task, sock_ctx *sk, void *data, size_t len, size_t *size, int32_t copy);
/// <summary>
/// 同步接收下一个响应包(不发送):用于一次请求产生多个响应的场景,如 MySQL 多结果集续读。
/// 同一连接上多协程并发的约束同 coro_send
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="size">输出:响应的 size，同 coro_send</param>
/// <returns>响应数据指针,仅在本协程下次挂起前有效(同 coro_send);超时/断开返回 NULL。
///   fd 为 INVALID_SOCK 时不挂起,直接返回 NULL——连接已 teardown 时挂上去等不到唤醒</returns>
void *coro_recv(task_ctx *task, sock_ctx *sk, size_t *size);
/// <summary>
/// 同步接收对端发来的下一条消息(不发送),不过协议层的回复判定:协议交上来的任意一条都会收到,
/// 是什么、怎么处理由调用方决定。同一连接上多协程并发的约束同 coro_send
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="size">输出:消息的 size，同 coro_send；可传 NULL</param>
/// <param name="slice">输出:分片标记(slice_type),0 为非分片完整消息,分片时调用方循环到 PROT_SLICE_END;
///   可传 NULL,返回 NULL 时写 0</param>
/// <returns>消息数据指针,有效期同 coro_send;断开返回 NULL;等满读超时(task_set_netread_timeout)会关掉连接再返回 NULL,
///   对端空闲时间可能超过读超时的服务端要先把它调大。fd 为 INVALID_SOCK 时不挂起,直接返回 NULL</returns>
void *coro_recv_any(task_ctx *task, sock_ctx *sk, size_t *size, uint8_t *slice);
/// <summary>
/// 等待分片消息。等待按 skid 排队、只认队头，而本函数每次调用都重新排到队尾——同一连接上
/// 有其他等待者时，后续分片会被派给别人，两边各拿到半截。整趟分片循环期间调用方须保证
/// 这条连接上没有并发的等待者
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="sk">连接标识</param>
/// <param name="size">本片的 size，同 coro_send；分片只算本片、不累计</param>
/// <param name="end">1 分片结束 0未结束；必须非 NULL，函数内裸解引用；
///   返回 NULL 时保证已写 0</param>
/// <returns>分片数据；仅在当前协程下次 yield（再调任意 coro_* API）前有效，
///   本协程下次挂起或结束时由框架释放，需要保留请自行拷贝</returns>
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
///   本协程下次挂起或结束时由框架释放，需要保留请自行拷贝。
///   发送失败 / 超时 / 连接已关返回 NULL</returns>
void *coro_sendto(task_ctx *task, sock_ctx *sk,
                  const char *ip, const uint16_t port,
                  void *data, size_t len, size_t *size, int32_t copy);
message_ctx *_coro_wait(task_ctx *task, uint64_t sess, msg_type mtype, uint32_t ms);

#endif//CORO_TASK_H_
