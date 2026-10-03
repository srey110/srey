#ifndef PROTS_H_
#define PROTS_H_

#include "event/evpub.h"
#include "protocol/prots_pub.h"

/// <summary>
/// 初始化协议模块，注册消息汇（网络事件回调经它向上推消息）
/// </summary>
/// <param name="emit">消息汇实现（begin/emit/end），prots 内部按值保存</param>
void prots_init(prot_emit *emit);
/// <summary>
/// 释放协议模块全局资源，并清掉 prots_init 注册的消息汇。
/// 清掉之后再触发任何网络事件回调都会当场崩——那本就是用已拆除 loader 的错，别让它静默跑下去。
/// 幂等，可重复调用；要恢复只能重新 prots_init
/// </summary>
void prots_free(void);
/// <summary>
/// 释放解包数据包内存，根据协议类型调用对应的释放函数
/// </summary>
/// <param name="pktype">协议包类型</param>
/// <param name="data">待释放的包指针</param>
void prots_pkfree(pack_type pktype, void *data);
/// <summary>
/// 释放udp解包数据包内存，根据协议类型调用对应的释放函数。
/// 当前所有 UDP 协议的包都是裸 MALLOC，函数体等价于一个 FREE，pktype 形参留给将来按协议分化
/// </summary>
/// <param name="pktype">协议包类型</param>
/// <param name="data">待释放的包指针</param>
void prots_udp_pkfree(pack_type pktype, void *data);
/// <summary>
/// 释放握手阶段数据包内存
/// </summary>
/// <param name="pktype">协议包类型</param>
/// <param name="data">待释放的包指针</param>
void prots_hsfree(pack_type pktype, void *data);
/// <summary>
/// 释放 ud_cxt 关联的协议上下文资源
/// </summary>
/// <param name="arg">ud_cxt 指针</param>
void prots_udfree(void *arg);
/// <summary>
/// 询问已解包的封包能否唤醒等待该 session 的协程。
/// 服务端主动推来的包不属于任何命令的响应,返非 OK 让框架改新建协程走 recv 回调,而不是按队头
/// 匹配挤掉真正的等待者:pgsql 的 PGPACK_NOTIFICATION、mqtt 的 PUBLISH/PUBREL/DISCONNECT。
/// WS 承载子协议时按其子协议判定
/// </summary>
/// <param name="pktype">协议包类型</param>
/// <param name="data">已解析的包数据;NULL 视为无包可拦</param>
/// <returns>ERR_OK=可唤醒等待者;其他值=不可,由调用方改新建协程处理</returns>
int32_t prots_may_resume(pack_type pktype, void *data);
/// <summary>
/// 统一解包入口，根据 ud->pktype 调用对应协议的解包函数
/// </summary>
/// <param name="ev">事件上下文</param>
/// <param name="sk">连接标识</param>
/// <param name="client">1=客户端 0=服务端</param>
/// <param name="buf">接收缓冲区</param>
/// <param name="ud">ud_cxt 指针</param>
/// <param name="size">输出：数据包长度。只有 DNS / SMTP / CUSTZ / NONE / UDP_KCP 会写；
///   HTTP / WEBSOCK / MQTT / REDIS / MYSQL / PGSQL / MONGO 的返回值是协议自己的 pack 对象，
///   长度恒为入口置的 0，要真实长度得走该协议的访问器（http_data / websock_data / ...）。
///   这个 0 会原样传到 _net_recv_cb 的 size 形参</param>
/// <param name="status">输出：解包状态标志</param>
/// <returns>解包后的数据指针，NULL 表示数据不足或出错</returns>
void *prots_unpack(ev_ctx *ev, sock_ctx *sk, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);
// 以下 7 个为网络事件回调，由 task_listen/connect/udp 装入 cbs_ctx、event 层触发；
// 各自经 prots_init 注册的消息汇 begin→emit→end 把事件转成 message_ctx 推给上层；
// 签名与 cbs_ctx 对应回调（accept_cb/connect_cb/recv_cb/...）一致。
/// <summary>接受新连接：完成协议初始化并推送 MSG_TYPE_ACCEPT</summary>
int32_t prots_net_accept(ev_ctx *ev, sock_ctx *sk, ud_cxt *ud);
/// <summary>主动连接建立：完成协议初始化并推送 MSG_TYPE_CONNECT</summary>
int32_t prots_net_connect(ev_ctx *ev, sock_ctx *sk, int32_t err, ud_cxt *ud);
/// <summary>数据接收：循环解包并推送 MSG_TYPE_RECV（按 slice 标记分片）</summary>
void prots_net_recv(ev_ctx *ev, sock_ctx *sk, int32_t client, buffer_ctx *buf, size_t size, ud_cxt *ud);
/// <summary>发送完成：推送 MSG_TYPE_SEND</summary>
void prots_net_send(ev_ctx *ev, sock_ctx *sk, int32_t client, size_t size, ud_cxt *ud);
/// <summary>SSL 握手完成：完成协议 SSL 初始化并推送 MSG_TYPE_SSLEXCHANGED</summary>
int32_t prots_net_ssl_exchanged(ev_ctx *ev, sock_ctx *sk, int32_t client, ud_cxt *ud, void *ssl);
/// <summary>连接关闭：通知协议层并推送 MSG_TYPE_CLOSE</summary>
/// <param name="erro">close_type，连接是怎么断的。CLOSE_TYPE_ORDERLY 与 CLOSE_TYPE_TRUNCATED 才认为
/// "body 由连接关闭界定"的那类消息收完了，其余一律不补末片；后者的末片是否可信由收到 CLOSE 的一方判</param>
void prots_net_close(ev_ctx *ev, sock_ctx *sk, int32_t client, int32_t erro, ud_cxt *ud);
/// <summary>UDP 接收：打包地址+数据并推送 MSG_TYPE_RECVFROM</summary>
void prots_net_recvfrom(ev_ctx *ev, sock_ctx *sk, char *buf, size_t size, netaddr_ctx *addr, ud_cxt *ud);
/// <summary>
/// 消息是否持有要释放的数据：广播共享的(shared 非 NULL)一律算；独占的看消息类型与 data 是否为空
/// </summary>
/// <param name="msg">消息</param>
/// <returns>ERR_OK 需要 message_clean；ERR_FAILED 不需要</returns>
int32_t message_should_clean(message_ctx *msg);
/// <summary>
/// 按消息类型释放 data：协议包走 prots_pkfree，UDP 包走 prots_udp_pkfree，握手数据走 prots_hsfree，
/// 跨 task 请求/响应直接 FREE；广播共享的只减引用，归 0 才释放。CLOSE 等不带数据的类型什么都不做
/// </summary>
/// <param name="msg">消息，释放后 data 不可再用</param>
void message_clean(message_ctx *msg);
/// <summary>
/// 该消息类型的等待条目在等待者摘空后是否保留（连接类与 UDP 类的 sess 是 skid，会反复复用）
/// </summary>
/// <param name="type">消息类型</param>
/// <returns>1 保留；0 摘空即删</returns>
int32_t message_may_keep(msg_type type);
/// <summary>
/// 消息类型的名字，日志与 dump 用
/// </summary>
/// <param name="type">消息类型</param>
/// <returns>名字；越界或名字表漏填时返回空串</returns>
const char *message_str(msg_type type);

#endif//PROTS_H_
