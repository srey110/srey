#ifndef TASK_H_
#define TASK_H_

#include "srey/spub.h"

// task 调度优先级上限（对应 x3 倍 n_base 加成；超此值 setter 自动 clamp）
#define TASK_PRIORITY_MAX  16
// 最大超时时间
#define TASK_TIMEOUT_MAX   (60 * 60 * 1000)
// 网络事件标志位（可按位组合）
typedef enum task_netev {
    NETEV_NONE   = 0x00, // 无额外事件
    NETEV_ACCEPT = 0x01, // 触发新连接接受回调
    NETEV_AUTHSSL= 0x02, // 触发 SSL 交换完成回调
    NETEV_SEND   = 0x04  // 触发数据发送完成回调
}task_netev;

/// <summary>
/// 新建任务；句柄由 createid 自动生成
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="name">字符串任务名；NULL 或空串表示匿名（仅有句柄，不进名表）</param>
/// <param name="quecap">消息队列容量（条数，一条 message_ctx 按值入队）；0 用默认 TASK_QUEUE_CAP。
/// 类型即底层 fsqu 的容量类型，
/// 调用方若持有更宽的值须自行判上界，不要靠这里截断</param>
/// <param name="_dispatch">消息分发函数, NULL默认分发函数</param>
/// <param name="_argfree">用户参数释放函数</param>
/// <param name="arg">用户参数</param>
/// <returns>task_ctx</returns>
task_ctx *task_new(loader_ctx *loader, const char *name, uint32_t quecap,
                   _task_dispatch_cb _dispatch, free_cb _argfree, void *arg);
/// <summary>
/// 任务释放
/// </summary>
/// <param name="task">task_ctx</param>
void task_free(task_ctx *task);
/// <summary>
/// 任务注册
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="_startup">任务初始化回调函数</param>
/// <param name="_closing">任务关闭回调函数,做业务相关收尾工作._closing执行后，不代表该任务已经无引用</param>
/// <returns>ERR_OK 成功。失败(名字重名)时 task 从未进过 loader 的表，所有权仍在调用方手上，
/// 须自行 task_free；此时调 task_ungrab 不会释放它，也不会有任何提示</returns>
int32_t task_register(task_ctx *task, _task_startup_cb _startup, _task_closing_cb _closing);
/// <summary>
/// 任务关闭 MSG_TYPE_CLOSING
/// </summary>
/// <param name="task">task_ctx</param>
void task_close(task_ctx *task);
/// <summary>
/// 检查任务是否正在关闭。
/// loader 全局关闭（loader->closing）或任务已被单独关闭（task->closing）均返回非 0。
/// 协程在发起耗时操作前可调用此函数提前终止，避免关闭流程中执行无意义操作。
/// </summary>
/// <param name="task">task_ctx</param>
/// <returns>非 0：正在关闭；0：运行中</returns>
int32_t task_isclosing(task_ctx *task);
/// <summary>
/// 获取任务类型。
/// </summary>
/// <param name="task">task_ctx</param>
/// <returns>task_type</returns>
task_type task_get_type(task_ctx *task);
/// <summary>
/// 获取任务,并使引用加一
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="handle">任务句柄</param>
/// <returns>task_ctx NULL未找到任务</returns>
task_ctx *task_grab(loader_ctx *loader, name_t handle);
/// <summary>
/// 按字符串名查找任务句柄（不增加引用计数；按名 grab 用 task_grab(loader, task_find_name(loader, name)) 组合）
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="name">字符串任务名</param>
/// <returns>name_t 句柄；未找到返回 INVALID_TNAME</returns>
name_t task_find_name(loader_ctx *loader, const char *name);
/// <summary>
/// 任务引用加一
/// </summary>
/// <param name="task">task_ctx</param>
void task_incref(task_ctx *task);
/// <summary>
/// 释放task_grab获取的任务,并使引用减一. 与 task_grab task_incref 配对
/// </summary>
/// <param name="task">task_ctx</param>
void task_ungrab(task_ctx *task);
/// <summary>
/// 把 task 绑到第 index 个 net 线程上执行：此后它的全部消息(网络、超时、请求、响应、启动、关闭)
/// 都在该线程本轮事件派发完之后跑，不再进 worker。任何时候可调，从下一次调度起生效。
/// 要让 startup 也在 net 线程上跑须在 task_register 之前调；由注册函数代为注册的 task(如协程 task)
/// 只能注册后再绑，startup 那一轮(连同绑定时已排进这一轮的消息)仍在 worker 上。已有连接不跟着迁：先绑再 connect / udp。
/// listen 不看绑定，accept 出的连接按 fd 分到各 net 线程；要让新连接落在处理它的 task 所在线程，用 task_accept_group。
/// 空闲时本线程上连接收到的完整包会在收包当场处理、不入队，回调里关连接推迟到收包回调返回后生效。
/// 回调里禁止阻塞与长计算：会卡住该线程上所有连接的收发，含别的 task 的连接
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="index">net 线程下标 [0, loader_nnet)；INVALID_INDEX 解绑、回到 worker</param>
/// <returns>ERR_OK 成功；index 越界返回 ERR_FAILED，绑定不变</returns>
int32_t task_bind_net(task_ctx *task, int32_t index);
/// <summary>
/// 获取 task 绑定的 net 线程
/// </summary>
/// <param name="task">task_ctx</param>
/// <returns>绑定的 net 线程下标；未绑定返回 INVALID_INDEX</returns>
int32_t task_net_index(task_ctx *task);
/// <summary>
/// 多核监听：每个 net 线程放一个处理连接的 task(组员)，由 head 来 listen。
/// 设好后 head 监听到的新连接，落在哪个 net 线程就交给那个线程的组员，head 自己不再收。
/// 只对之后的 listen 生效，只能设一次；head 退出后新连接一律拒收。
/// 每个组员的 arg 要各给一份(共用一份会被重复释放)；组员间共享的数据用原子量，或交给一个不绑线程的 task 汇总
/// </summary>
/// <param name="head">发起 listen 的 task，可以是组员之一，也可以是别的 task</param>
/// <param name="handles">组员句柄，handles[i] 须已用 task_bind_net 绑到第 i 个 net 线程；函数内会拷一份</param>
/// <param name="n">组员个数，须等于 loader_nnet</param>
/// <returns>ERR_OK 成功；个数不对、某个组员不在或没绑对线程、已经设过，返回 ERR_FAILED</returns>
int32_t task_accept_group(task_ctx *head, const name_t *handles, uint16_t n);
/// <summary>
/// 超时：ms 毫秒后触发。sess 与 _timeout 互斥,须恰好一个有效(违反将 ASSERTAB abort)：
/// _timeout≠NULL 且 sess=0 → 到期回调 _timeout(task, 0)；
/// _timeout=NULL 且 sess≠0 → 到期投递 TIMEOUT 消息,按 sess 唤醒等待协程(无等待者则无操作)
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="sess">session；消息(唤醒)模式须非 0,回调模式须为 0</param>
/// <param name="ms">毫秒</param>
/// <param name="_timeout">超时回调函数；回调模式须非 NULL,消息模式须为 NULL</param>
void task_timeout(task_ctx *task, uint64_t sess, uint32_t ms, _timeout_cb _timeout);
/// <summary>
/// 任务间通信,请求。src 与 sess 须同真同假(违反将 ASSERTAB abort)：
/// src≠NULL 且 sess≠0 → dst 可经 task_response 按 sess 回复 src；
/// src=NULL 且 sess=0 → 单向请求,dst 无从回复
/// </summary>
/// <param name="dst">目标任务</param>
/// <param name="src">发起者；需响应时非 NULL(且 sess≠0),单向时 NULL(且 sess=0)</param>
/// <param name="reqtype">请求类型 request_type</param>
/// <param name="sess">session；需响应时非 0,单向时 0</param>
/// <param name="data">数据</param>
/// <param name="size">数据长度</param>
/// <param name="copy">1 拷贝 0不拷贝</param>
void task_request(task_ctx *dst, task_ctx *src, subtype_t reqtype, uint64_t sess,
                  void *data, size_t size, int32_t copy);
/// <summary>
/// 任务间通信,返回
/// </summary>
/// <param name="dst">目标任务</param>
/// <param name="reqtype">请求类型 request_type</param>
/// <param name="sess">session</param>
/// <param name="erro">错误码</param>
/// <param name="data">数据</param>
/// <param name="size">数据长度</param>
/// <param name="copy">1 拷贝 0不拷贝</param>
void task_response(task_ctx *dst, subtype_t reqtype, uint64_t sess,
                   int32_t erro, void *data, size_t size, int32_t copy);
/// <summary>
/// 任务间通信,无返回
/// </summary>
/// <param name="dst">目标任务</param>
/// <param name="reqtype">请求类型 request_type</param>
/// <param name="data">数据</param>
/// <param name="size">数据长度</param>
/// <param name="copy">1 拷贝 0不拷贝</param>
void task_call(task_ctx *dst, subtype_t reqtype, void *data, size_t size, int32_t copy);
/// <summary>
/// 按句柄投递请求，语义同 task_request，但不用调用方先 grab 目标：查表与投递在同一段读锁内完成，
/// 省掉对目标 task 引用计数的一加一减。查表前先按 copy 组好消息
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="handle">目标任务句柄；INVALID_TNAME 直接失败</param>
/// <param name="src">同 task_request</param>
/// <param name="reqtype">同 task_request</param>
/// <param name="sess">同 task_request</param>
/// <param name="data">数据</param>
/// <param name="size">数据长度</param>
/// <param name="copy">1 拷贝 0不拷贝</param>
/// <returns>ERR_OK 已投递，data 按 copy 语义交出；ERR_FAILED 目标不存在：copy=1 时内部副本已释放，
/// copy=0 时 data 没碰过、仍归调用方（可换句柄重试或自行释放）</returns>
int32_t task_request_to(loader_ctx *loader, name_t handle, task_ctx *src, subtype_t reqtype, uint64_t sess,
                        void *data, size_t size, int32_t copy);
/// <summary>
/// 按句柄投递响应，语义同 task_response，投递方式同 task_request_to
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="handle">目标任务句柄；INVALID_TNAME 直接失败</param>
/// <param name="reqtype">请求类型 request_type</param>
/// <param name="sess">session</param>
/// <param name="erro">错误码</param>
/// <param name="data">数据</param>
/// <param name="size">数据长度</param>
/// <param name="copy">1 拷贝 0不拷贝</param>
/// <returns>同 task_request_to</returns>
int32_t task_response_to(loader_ctx *loader, name_t handle, subtype_t reqtype, uint64_t sess,
                         int32_t erro, void *data, size_t size, int32_t copy);
/// <summary>
/// 按句柄投递单向调用，语义同 task_call，投递方式同 task_request_to
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="handle">目标任务句柄；INVALID_TNAME 直接失败</param>
/// <param name="reqtype">请求类型 request_type</param>
/// <param name="data">数据</param>
/// <param name="size">数据长度</param>
/// <param name="copy">1 拷贝 0不拷贝</param>
/// <returns>同 task_request_to</returns>
int32_t task_call_to(loader_ctx *loader, name_t handle, subtype_t reqtype, void *data, size_t size, int32_t copy);
/// <summary>
/// 广播请求：把同一份 data 投递给 N 个 task,各 dst 在 _request 回调中可独立 task_response 回 src(共用同一 sess)。
/// 与 task_multi_call 区别：携带 src + sess,dst 知道响应该回给谁；src 端 _response 回调将被调用 N 次
/// （同 sess,用户自行累计/区分,框架不做应答聚合）。src=NULL && sess=0 时退化为 task_multi_call 语义。
/// data 由内部引用计数共享,最后一个消费者释放；dsts[i]=NULL 占位跳过,有效 dst 数为 0 时按 copy 语义清理 data。
/// </summary>
/// <param name="dsts">目标 task 数组(调用方保证每个 dst 生命周期到投递返回)</param>
/// <param name="n">数组长度</param>
/// <param name="src">请求发起者(非 NULL),响应将通过 task_response 回到该 task；NULL 时走 fire-and-forget(sess 须为 0)</param>
/// <param name="reqtype">请求类型 request_type</param>
/// <param name="sess">会话 id(非 0),N 个 dst 共用此 sess,src 自行据此识别广播响应；src=NULL 时须为 0</param>
/// <param name="data">数据</param>
/// <param name="size">数据长度</param>
/// <param name="copy">1 拷贝(内部 MALLOC+memcpy)；0 不拷贝(直接转移 data 所有权)</param>
/// <returns>实际成功投递的 dst 数（dsts 中非 NULL 元素个数,0 表示全部跳过未投递）</returns>
int32_t task_multi_request(task_ctx *dsts[], int32_t n, task_ctx *src, subtype_t reqtype,
                           uint64_t sess, void *data, size_t size, int32_t copy);
/// <summary>
/// 单向投递同一份数据给 N 个 task(fire-and-forget pub/sub,publisher 不等响应)。
/// data 由内部引用计数共享,最后一个消费者释放,比 N 次 task_call 省 N-1 份拷贝。
/// dsts[i]=NULL 占位跳过;有效 dst 数为 0 时按 copy 语义清理 data。
/// </summary>
/// <param name="dsts">目标 task 数组(调用方保证每个 dst 生命周期到投递返回)</param>
/// <param name="n">数组长度</param>
/// <param name="reqtype">请求类型 request_type</param>
/// <param name="data">数据</param>
/// <param name="size">数据长度</param>
/// <param name="copy">1 拷贝(内部 MALLOC+memcpy)；0 不拷贝(直接转移 data 所有权)</param>
void task_multi_call(task_ctx *dsts[], int32_t n, subtype_t reqtype,
                     void *data, size_t size, int32_t copy);
/// <summary>
/// 监听。task 设过 task_accept_group 的，新连接交给连接所在 net 线程的组员
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="pktype">包类型</param>
/// <param name="evssl">evssl_ctx</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="id">监听ID</param>
/// <param name="netev">task_netev</param>
/// <returns>ERR_OK 成功</returns>
int32_t task_listen(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, uint64_t *id, int32_t netev);
/// <summary>
/// 链接。task 绑了 net 线程(task_bind_net)的，连接落在同一条线程上，之后改绑不跟着迁
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="pktype">包类型</param>
/// <param name="evssl">evssl_ctx</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="netev">task_netev</param>
/// <param name="extra">ud_cxt extra</param>
/// <param name="setsess">是否设置sess</param>
/// <param name="sk">连接标识</param>
/// <returns>ERR_OK 成功</returns>
int32_t task_connect(task_ctx *task, pack_type pktype, struct evssl_ctx *evssl,
    const char *ip, uint16_t port, int32_t netev, void *extra, int32_t setsess, sock_ctx *sk);
/// <summary>
/// UDP。落在哪个 net 线程同 task_connect
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="pktype">包类型</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="sk">连接标识</param>
/// <returns>ERR_OK 成功</returns>
int32_t task_udp(task_ctx *task, pack_type pktype, const char *ip, uint16_t port, sock_ctx *sk);
/// <summary>
/// 设置 task 调度优先级。priority 越大,worker 单次消费消息越多:
/// n = n_base * (1 + priority/8),cap 到当前队列长度,n_base 由 worker.weight 推导。
/// 默认 0;可在任意线程随时调整,下一轮调度生效。
/// work-stealing 偷过去后 priority 跟随 task,但 stealer 的 weight 决定 n_base。
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="priority">建议 0..TASK_PRIORITY_MAX (16);负值 clamp 到 0,超过 clamp 到 TASK_PRIORITY_MAX</param>
void task_set_priority(task_ctx *task, int32_t priority);
/// <summary>
/// 获取 task 当前调度优先级
/// </summary>
/// <param name="task">task_ctx</param>
/// <returns>当前 priority (0..TASK_PRIORITY_MAX)</returns>
int32_t task_get_priority(task_ctx *task);
/// <summary>
/// 设置task_request超时时间
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="ms">毫秒，须 大于 0（0 忽略本次设置）；超过 TASK_TIMEOUT_MAX 时 clamp 到该上界</param>
void task_set_request_timeout(task_ctx *task, uint32_t ms);
/// <summary>
/// 获取task_request超时时间
/// </summary>
/// <param name="task">task_ctx</param>
/// <returns>毫秒</returns>
uint32_t task_get_request_timeout(task_ctx *task);
/// <summary>
/// 设置task_connect超时时间
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="ms">毫秒，须 大于 0（0 忽略本次设置）；超过 TASK_TIMEOUT_MAX 时 clamp 到该上界</param>
void task_set_connect_timeout(task_ctx *task, uint32_t ms);
/// <summary>
/// 获取task_connect超时时间
/// </summary>
/// <param name="task">task_ctx</param>
/// <returns>毫秒</returns>
uint32_t task_get_connect_timeout(task_ctx *task);
/// <summary>
/// 设置网络读取超时时间
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="ms">毫秒，须 大于 0（0 忽略本次设置）；超过 TASK_TIMEOUT_MAX 时 clamp 到该上界</param>
void task_set_netread_timeout(task_ctx *task, uint32_t ms);
/// <summary>
/// 获取网络读取超时时间
/// </summary>
/// <param name="task">task_ctx</param>
/// <returns>毫秒</returns>
uint32_t task_get_netread_timeout(task_ctx *task);
/// <summary>
/// 获取任务自启动以来按消息类型分桶的累计消息条数与 dispatch 占用的线程 CPU 时间。
/// 用于排查"哪类消息消耗 CPU"：IO 等待 / coro_sleep / 被抢占的时间不计入；
/// 由 mtype 索引（MSG_TYPE_NONE 槽未使用），可据此估算单消息平均 CPU 耗时。
/// 只能在该 task 自己的消息回调里调（含它的协程）：统计由正在分发它的线程不加锁地累加，别的线程读是数据竞争
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="nmsg">出参数组（长度须为 MSG_TYPE_ALL），按 mtype 索引累计消息条数（仅编译期开启 ENABLE_DISPATCH_STAT 时累加，否则恒为 0）</param>
/// <param name="dispatch_cpu_ns">出参数组（长度须为 MSG_TYPE_ALL），按 mtype 索引累计 dispatch 占用线程 CPU 纳秒（同上）</param>
void task_stat(task_ctx *task, uint64_t nmsg[MSG_TYPE_ALL], uint64_t dispatch_cpu_ns[MSG_TYPE_ALL]);
/// <summary>
/// 注册新连接接受回调函数
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="_accept">_net_accept_cb 回调函数</param>
void task_accepted(task_ctx *task, _net_accept_cb _accept);
/// <summary>
/// 注册数据接收回调函数
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="_recv">_net_recv_cb 回调函数</param>
void task_recved(task_ctx *task, _net_recv_cb _recv);
/// <summary>
/// 注册数据发送完成回调函数
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="_send">_net_send_cb 回调函数</param>
void task_sended(task_ctx *task, _net_send_cb _send);
/// <summary>
/// 注册连接建立回调函数
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="_connect">_net_connect_cb 回调函数</param>
void task_connected(task_ctx *task, _net_connect_cb _connect);
/// <summary>
/// 注册 SSL 交换完成回调函数
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="_exchanged">_net_ssl_exchanged_cb 回调函数</param>
void task_ssl_exchanged(task_ctx *task, _net_ssl_exchanged_cb _exchanged);
/// <summary>
/// 注册应用层握手完成回调函数
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="_handshake">_net_handshake_cb 回调函数</param>
void task_handshaked(task_ctx *task, _net_handshake_cb _handshake);
/// <summary>
/// 注册连接关闭回调函数。回调的 erro 是 close_type，说明连接是怎么断的：TLS 少发 close_notify
/// 的 CLOSE_TYPE_TRUNCATED 下，"由连接关闭界定 body"的那类协议末片照给但可能被截断，
/// 收不收由本回调按 erro 自行判（判据见 prots_net_close）。CLOSE_TYPE_NEVERCONN 不会投到
/// 本回调——连接从未建立，失败已由 CONNECT（TCP）或 HANDSHAKED（KCP）的 erro 交出
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="_close">_net_close_cb 回调函数</param>
void task_closed(task_ctx *task, _net_close_cb _close);
/// <summary>
/// 注册 UDP 数据接收回调函数
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="_recvfrom">_net_recvfrom_cb 回调函数</param>
void task_recvedfrom(task_ctx *task, _net_recvfrom_cb _recvfrom);
/// <summary>
/// 注册任务间请求回调函数
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="_request">_request_cb 回调函数</param>
void task_requested(task_ctx *task, _request_cb _request);
/// <summary>
/// 注册任务间响应回调函数
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="_response">_response_cb 回调函数</param>
void task_responsed(task_ctx *task, _response_cb _response);
/// <summary>
/// 注册一轮调度结束回调：worker 处理完一轮消息、交还调度权之前调用，调用时独占该 task。
/// 只能在 task 开始接收消息前设置
/// </summary>
/// <param name="task">task_ctx</param>
/// <param name="_round_end">_task_round_end_cb 回调函数；NULL 不回调（默认）</param>
void task_round_ended(task_ctx *task, _task_round_end_cb _round_end);

#endif//TASK_H_
