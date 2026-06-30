#include "task_tcp_server.h"

static void *_evssl = NULL;
static uint16_t _port = 0;
static int32_t _prt = 0;
static name_t _rpcname = INVALID_TNAME;

// 新连接接入
static void _net_accept(task_ctx *task, sk_id *sk, subtype_t pktype) {
    (void)task;
    (void)pktype;
    if (_prt) {
        LOG_INFO("accept socket %d.", (uint32_t)sk->fd);
    }
}
// SSL 握手完成
static void _net_ssl_exchanged(task_ctx *task, sk_id *sk, subtype_t pktype, uint8_t client) {
    (void)task;
    (void)pktype;
    (void)client;
    if (_prt) {
        LOG_INFO("ssl_exchanged socket %d.", (uint32_t)sk->fd);
    }
}
// 收到数据包，按首字节指令分发处理
static void _net_recv(task_ctx *task, sk_id *sk, subtype_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
    (void)slice;
    binary_ctx reader;
    binary_init(&reader, data, size, 0);
    int8_t prot = binary_get_int8(&reader);
    switch (prot) {
    case TEST_ECHO: {
        // 原样回显整个数据包
        size_t lens;
        void *outbuf = custz_pack(pktype, data, size, &lens);
        ev_send(&task->loader->netev, sk->fd, sk->skid, outbuf, lens, 0);
        break;
    }
    case TEST_SSL_CHANGE: {
        // 先回显告知客户端可以开始握手，再投 CMD_SSL；
        // 两条命令顺序入同一 watcher 队列，客户端收到回显时
        // CMD_SSL 必然已处理完毕，服务端已就绪，避免多任务并发时握手失败
        size_t lens;
        void *outbuf = custz_pack(pktype, data, size, &lens);
        ev_send(&task->loader->netev, sk->fd, sk->skid, outbuf, lens, 0);
        if (ERR_OK != ev_ssl(&task->loader->netev, sk->fd, sk->skid, client, _evssl)) {
            LOG_WARN("ev_ssl error.");
            ev_close(&task->loader->netev, sk->fd, sk->skid, 1);
        }
        break;
    }
    case TEST_PKTYPE_CHANGE: {
        // 先回显当前协议格式的确认包，再切换本端 pack_type；
        // 客户端收到回显后同步切换，保证双端同步
        uint8_t type = (uint8_t)binary_get_int8(&reader);
        size_t lens;
        void *outbuf = custz_pack(pktype, data, size, &lens);
        ev_send(&task->loader->netev, sk->fd, sk->skid, outbuf, lens, 0);
        ev_ud_pktype(&task->loader->netev, sk->fd, sk->skid, type);
        break;
    }
    case TEST_RPC_ECHO: {
        // 将整包数据（含命令字节）转发给 task_rpc type 2 回显，再原样返回给客户端；
        // coro_request 在此处 yield，需要 coro_task_register 提供协程上下文
        task_ctx *rpc = task_grab(task->loader, _rpcname);
        if (NULL == rpc) {
            LOG_WARN("grab rpc task error.");
            ev_close(&task->loader->netev, sk->fd, sk->skid, 1);
            break;
        }
        int32_t erro;
        size_t rlen;
        void *echo = coro_request(rpc, task, 101, data, size, 1, &erro, &rlen);
        task_ungrab(rpc);
        if (ERR_OK != erro || NULL == echo || rlen != size) {
            LOG_WARN("rpc echo error.");
            ev_close(&task->loader->netev, sk->fd, sk->skid, 1);
            break;
        }
        size_t lens;
        void *outbuf = custz_pack(pktype, echo, rlen, &lens);
        ev_send(&task->loader->netev, sk->fd, sk->skid, outbuf, lens, 0);
        break;
    }
    default: {
        // 未知指令一律回显
        size_t lens;
        void *outbuf = custz_pack(pktype, data, size, &lens);
        ev_send(&task->loader->netev, sk->fd, sk->skid, outbuf, lens, 0);
        break;
    }
    }
}
// 数据发送完成
static void _net_send(task_ctx *task, sk_id *sk, subtype_t pktype, uint8_t client, size_t size) {
    (void)task;
    (void)pktype;
    (void)client;
    if (_prt) {
        LOG_INFO("socket %d sended %d byte.", (uint32_t)sk->fd, (uint32_t)size);
    }
}
// 连接关闭
static void _net_close(task_ctx *task, sk_id *sk, subtype_t pktype, uint8_t client) {
    (void)task;
    (void)pktype;
    (void)client;
    if (_prt) {
        LOG_INFO("socket %d closed", (uint32_t)sk->fd);
    }
}
static void _startup(task_ctx *task) {
    task_accepted(task, _net_accept);
    task_ssl_exchanged(task, _net_ssl_exchanged);
    task_recved(task, _net_recv);
    task_sended(task, _net_send);
    task_closed(task, _net_close);
    uint64_t id;
    // 以 PACK_CUSTZ_FIXED 协议启动监听，运行中可通过 TEST_PKTYPE_CHANGE 动态切换
    if (ERR_OK != task_listen(task, PACK_CUSTZ_FIXED, NULL, "0.0.0.0", _port, &id,
        NETEV_ACCEPT | NETEV_SEND | NETEV_AUTHSSL)) {
        LOG_WARN("task_listen %d error.", _port);
    }
}
void task_tcp_erver_start(loader_ctx *loader, const char *name, uint16_t port,
    void *evssl, const char *rpcname, int32_t pt) {
    _port = port;
    _evssl = evssl;
    _rpcname = task_find_name(loader, rpcname);
    _prt = pt;
    coro_task_register(loader, name, 0, _startup, NULL, NULL, NULL);
}
