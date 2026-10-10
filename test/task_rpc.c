#include "task_rpc.h"

#define EVTASK_REQ    110// 发给绑定 task：回它处理请求时所在的 net 线程下标
#define EVTASK_REQ_CO 111// 发给绑定协程 task：它先回头请求本 task，醒来后再回所在线程下标
#define EVTASK_PORT   15021// 绑定 task 的回显端口
#define EVTASK_REQ_CONN 112// 发给绑定协程 task：它主动连回显端口收发一次，回连接所在线程下标
#define ACP_GROUP_PORT 15024// 接收组的监听端口
#define EVTASK_WS_PORT 15025// websocket 接收组的端口：握手请求与首帧同一次到达
#define EVTASK_SW_PORT 15026// 组员收包当场切协议的端口，末尾组头退出后再连应被拒
#define EVTASK_UDP_PORT 15027// 绑定协程 task 建 UDP 用的端口
#define EVTASK_HO_PORT 15031// 组员收包当场把连接转给别的 task 的端口

static int32_t _prt = 0;
static atomic_t _emit_recved;// 消息汇嵌套用例里目标 task 收到的条数
static atomic_t _evt_started;// 绑定 task 的 startup 跑过
static atomic_t _evt_bad;// 绑定 task 的回调跑在别的线程上的次数
static atomic_t _evtco_started;// 绑定协程 task 的 startup 跑过
static atomic_t _evt_timeouted;// 绑定 task 的 task_timeout 回调跑过
static atomic_t _sw_switched;// 切协议用例：首包按 custz 收到并切成 PACK_NONE 的次数
static atomic_t _sw_rest;// 切协议用例：同一次读到的后续字节按 PACK_NONE 收到的次数
static atomic_t _sw_bad;// 切协议用例：收到意料之外的包的次数
static atomic_t _member_accepted;// 组员收到的 ACCEPT 数
static atomic_t _member_bad;// 组员收到不归自己线程的连接、或回调跑错线程的次数
static atomic_t _ws_handshaked;// 绑定 ws task 收到成功握手的次数
static atomic_t _ws_recved;// 绑定 ws task 收到数据帧的次数
static atomic_t _ws_disorder;// 数据帧乱序到达(抢在握手结果或先到的分片前面)的次数
static atomic_t _ho_switched;// 转交用例：组员收到首包并转走连接的次数
static atomic_t _ho_rest;// 转交用例：新目标收到同一次读到的后续包的次数
static atomic_t _ho_bad;// 转交用例：收到意料之外的包的次数

// 整数加法，供 type1 请求调用
static int32_t _add(int32_t a, int32_t b) {
    return a + b;
}
// 处理来自 task_timeout 的 RPC 请求，src 为 INVALID_TNAME 时表示 fire-and-forget
static void _requested(task_ctx *task, subtype_t reqtype, uint64_t sess, name_t src, void *data, size_t size) {
    switch (reqtype) {
    case 100: {
        // 整数加法：读取两个 int32（网络字节序），返回和（网络字节序）
        binary_ctx breader;
        binary_init_read(&breader, data, size);
        int32_t a = (int32_t)binary_get_integer(&breader, 4, 0);
        int32_t b = (int32_t)binary_get_integer(&breader, 4, 0);
        int sum = _add(a, b);
        if (INVALID_TNAME != src) {
            task_ctx *resp = task_grab(task->loader, src);
            if (NULL != resp) {
                int32_t rst = htonl(sum);
                task_response(resp, reqtype, sess, ERR_OK, &rst, sizeof(rst), 1);
                task_ungrab(resp);
            } else {
                LOG_WARN("grab task %"PRIu64" error.", src);
            }
        } else {
            // task_call fire-and-forget，无需回复
            if (_prt) {
                LOG_INFO("this is task call, sum: %d", sum);
            }
        }
        break;
    }
    case 101: {
        // 字节串回显：原样返回请求数据，覆盖变长数据路径；按句柄投递（task_response_to），不先 grab
        if (INVALID_TNAME != src
            && ERR_OK != task_response_to(task->loader, src, reqtype, sess, ERR_OK, data, size, 1)) {
            LOG_WARN("response to task %"PRIu64" error.", src);
        }
        break;
    }
    default:
        break;
    }
}
// 按句柄投递的失败契约：目标不在返回 ERR_FAILED，copy=1 时内部副本自己放、copy=0 时载荷没碰过仍归调用方；
// 投给自己的单向加法走成功路径。泄漏与重复释放由退出时的内存检查兜
static void _check_post_to(task_ctx *task) {
    int32_t pair[2];
    name_t gone = createid();// 从没注册过的句柄
    void *buf;
    pair[0] = (int32_t)htonl(1);
    pair[1] = (int32_t)htonl(2);
    ASSERTAB(ERR_FAILED == task_call_to(task->loader, gone, 100, pair, sizeof(pair), 1),
             "task_call_to unknown handle must fail");
    ASSERTAB(ERR_FAILED == task_response_to(task->loader, INVALID_TNAME, 100, 1, ERR_OK, pair, sizeof(pair), 1),
             "task_response_to invalid handle must fail");
    buf = dup_zero(pair, sizeof(pair));
    ASSERTAB(ERR_FAILED == task_request_to(task->loader, gone, NULL, 100, 0, buf, sizeof(pair), 0),
             "task_request_to unknown handle must fail");
    FREE(buf);// copy=0 投递失败，载荷仍归调用方
    ASSERTAB(ERR_OK == task_call_to(task->loader, task->handle, 100, pair, sizeof(pair), 1),
             "task_call_to self must succeed");
}
// 消息汇嵌套用例的目标 task：收到即记一笔
static void _emit_target_recv(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client,
                              uint8_t slice, void *data, size_t size) {
    (void)task; (void)sk; (void)pktype; (void)client; (void)slice; (void)data; (void)size;
    ATOMIC_ADD(&_emit_recved, 1);
}
static void _emit_target_startup(task_ctx *task) {
    task_recved(task, _emit_target_recv);
}
// 消息汇窗口嵌套：外层已推过消息，内层对不存在的目标开窗失败，外层关窗时仍得激活目标 task。
// event 线程解包时同线程关掉别的连接、而那条连接的 task 已退出，就是这个形状
static void _check_emit_nested(void *owner, void *arg) {
    task_ctx *task = owner;
    prot_emit *emit = _task_net_emit();
    message_ctx msg;
    task_ctx *dst;
    name_t handle;
    void *target;
    int32_t i;
    (void)arg;
    dst = task_new(task->loader, "rpc_emit_target", 0, NULL, NULL, NULL);
    handle = dst->handle;
    if (ERR_OK != task_register(dst, _emit_target_startup, NULL)) {
        task_free(dst);
        ASSERTAB(0, "register emit target failed");
    }
    coro_sleep(task, 100);// 等目标跑完 STARTUP 闲下来，之后只有关窗那次激活能让它再跑
    target = emit->begin(task->loader, handle);
    ASSERTAB(NULL != target, "open emit window failed");
    ZERO(&msg, sizeof(msg));
    msg.mtype = MSG_TYPE_RECV;
    msg.subtype = PACK_NONE;
    msg.data = dup_zero("x", 1);
    msg.size = 1;
    emit->emit(target, &msg);
    ASSERTAB(NULL == emit->begin(task->loader, createid()), "unknown handle must not open a window");
    emit->end(target);
    for (i = 0; i < 100 && 0 == ATOMIC_GET(&_emit_recved); i++) {
        coro_sleep(task, 10);
    }
    ASSERTAB(1 == ATOMIC_GET(&_emit_recved), "outer emit window lost its activation");
}
// 绑定 task 的回调都得跑在它绑的 net 线程 0 上
static void _evt_check_thread(task_ctx *task) {
    if (0 != ev_cur_index(&task->loader->netev)) {
        ATOMIC_ADD(&_evt_bad, 1);
    }
}
static void _evt_requested(task_ctx *task, subtype_t reqtype, uint64_t sess, name_t src, void *data, size_t size) {
    int32_t idx = ev_cur_index(&task->loader->netev);
    (void)data; (void)size;
    if (EVTASK_REQ == reqtype && INVALID_TNAME != src) {
        (void)task_response_to(task->loader, src, reqtype, sess, ERR_OK, &idx, sizeof(idx), 1);
    }
}
static void _evt_recv(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client,
                      uint8_t slice, void *data, size_t size) {
    (void)pktype; (void)client; (void)slice;
    _evt_check_thread(task);
    ev_send(&task->loader->netev, sk, data, size, 1);
}
static void _evt_timeout(task_ctx *task, uint64_t sess) {
    (void)sess;
    _evt_check_thread(task);
    ATOMIC_SET(&_evt_timeouted, 1);
}
static void _evt_startup(task_ctx *task) {
    uint64_t id;
    _evt_check_thread(task);
    task_requested(task, _evt_requested);
    task_recved(task, _evt_recv);
    task_timeout(task, 0, 10, _evt_timeout);
    ASSERTAB(ERR_OK == task_listen(task, PACK_NONE, NULL, "127.0.0.1", EVTASK_PORT, &id, 0), "evtask listen failed");
    ATOMIC_SET(&_evt_started, 1);
}
// 绑定协程 task 主动连接、建 UDP：都应落在它绑的 net 线程上，收发照常。两者不同线程返回 -1
static int32_t _evtco_connect_index(task_ctx *task) {
    sock_ctx sk, usk;
    size_t rlens;
    void *rt;
    int32_t idx = -1;
    if (ERR_OK != coro_connect(task, PACK_NONE, NULL, "127.0.0.1", EVTASK_PORT, 0, NULL, &sk)) {
        return -1;
    }
    rt = coro_send(task, &sk, "evtco", 5, &rlens, 1);
    if (NULL != rt && 5 == rlens && 0 == memcmp(rt, "evtco", 5)) {
        idx = sk.index;
    }
    ev_close(&task->loader->netev, &sk);
    if (ERR_OK != task_udp(task, PACK_NONE, "127.0.0.1", EVTASK_UDP_PORT, &usk)) {
        return -1;
    }
    if (usk.index != idx) {
        idx = -1;
    }
    ev_close(&task->loader->netev, &usk);
    return idx;
}
// 绑定协程 task 收到请求：回头请求 worker 上的本 task(单向加法那条)，挂起后应在原 net 线程上醒来
static void _evtco_requested(task_ctx *task, subtype_t reqtype, uint64_t sess, name_t src, void *data, size_t size) {
    int32_t pair[2];
    int32_t idx = -1;
    int32_t erro;
    size_t rlens;
    void *rt;
    task_ctx *peer;
    (void)data; (void)size;
    if (EVTASK_REQ_CONN == reqtype && INVALID_TNAME != src) {
        idx = _evtco_connect_index(task);
        (void)task_response_to(task->loader, src, reqtype, sess, ERR_OK, &idx, sizeof(idx), 1);
        return;
    }
    if (EVTASK_REQ_CO != reqtype || INVALID_TNAME == src) {
        return;
    }
    _evt_check_thread(task);
    peer = task_grab(task->loader, src);
    if (NULL != peer) {
        pair[0] = (int32_t)htonl(1);
        pair[1] = (int32_t)htonl(2);
        rt = coro_request(peer, task, 100, pair, sizeof(pair), 1, &erro, &rlens);
        task_ungrab(peer);
        if (NULL != rt && ERR_OK == erro && sizeof(int32_t) == rlens
            && 3 == (int32_t)ntohl(*(uint32_t *)rt)) {
            coro_sleep(task, 10);// 再挂起一次，换成定时器唤醒
            idx = ev_cur_index(&task->loader->netev);
        }
    }
    (void)task_response_to(task->loader, src, reqtype, sess, ERR_OK, &idx, sizeof(idx), 1);
}
// 一直挂在 sleep 上直到停机：每次醒来都在绑定的 net 线程上，停机时挂着的协程不能卡住退出
static void _evtco_park(void *owner, void *arg) {
    task_ctx *task = owner;
    (void)arg;
    while (!task_isclosing(task)) {
        coro_sleep(task, 50);
        ASSERTAB(0 == ev_cur_index(&task->loader->netev), "parked bound coro woke off its net thread");
    }
}
static void _evtco_startup(task_ctx *task) {
    task_requested(task, _evtco_requested);
    coro_fork(coro_task_co(task), _evtco_park, NULL);
    ATOMIC_SET(&_evtco_started, 1);
}
// 组员：连接得落在本组员绑的线程上，回调也在那条线程上跑
static void _member_check(task_ctx *task, sock_ctx *sk) {
    if (sk->index != task_net_index(task)
        || ev_cur_index(&task->loader->netev) != sk->index) {
        ATOMIC_ADD(&_member_bad, 1);
    }
}
static void _member_accept(task_ctx *task, sock_ctx *sk, subtype_t pktype) {
    (void)pktype;
    _member_check(task, sk);
    ATOMIC_ADD(&_member_accepted, 1);
}
static void _member_recv(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client,
                        uint8_t slice, void *data, size_t size) {
    (void)pktype; (void)client; (void)slice;
    _member_check(task, sk);
    ev_send(&task->loader->netev, sk, data, size, 1);
}
static void _member_startup(task_ctx *task) {
    task_accepted(task, _member_accept);
    task_recved(task, _member_recv);
}
// 每个 net 线程建一个绑在本线程上的组员 task，句柄按线程下标写进 hs
static void _group_new(task_ctx *task, const char *prefix, _task_startup_cb startup, name_t *hs) {
    uint32_t nnet = loader_nnet(task->loader);
    uint32_t i;
    task_ctx *member;
    char name[64];
    for (i = 0; i < nnet; i++) {
        SNPRINTF(name, sizeof(name), "%s_%u", prefix, i);
        member = task_new(task->loader, name, 0, NULL, NULL, NULL);
        ASSERTAB(ERR_OK == task_bind_net(member, (int32_t)i), "bind member failed");
        hs[i] = member->handle;
        if (ERR_OK != task_register(member, startup, NULL)) {
            task_free(member);
            ASSERTAB(0, "register member failed");
        }
    }
}
// 接收组：每个 net 线程一个绑在本线程的组员，accept 时按连接所在线程交给同线程的组员；
// 表的条数、组员的绑定不对与重复设置都拒绝
static void _check_accept_group(task_ctx *task) {
    uint32_t nnet = loader_nnet(task->loader);
    uint32_t i;
    name_t *hs;
    task_ctx *head;
    sock_ctx sk;
    uint64_t id;
    size_t rlens;
    void *rt;
    MALLOC(hs, sizeof(name_t) * (nnet + 1));
    _group_new(task, "rpc_member", _member_startup, hs);
    hs[nnet] = task->handle;
    coro_sleep(task, 100);// 等各组员跑完 STARTUP
    head = task_grab(task->loader, hs[0]);
    ASSERTAB(NULL != head, "grab group head failed");
    ASSERTAB(ERR_FAILED == task_accept_group(head, hs, (uint16_t)(nnet + 1)), "member count must equal nnet");
    ASSERTAB(ERR_FAILED == task_accept_group(head, hs + 1, (uint16_t)nnet), "member bound to another thread must fail");
    ASSERTAB(ERR_OK == task_accept_group(head, hs, (uint16_t)nnet), "set accept group failed");
    ASSERTAB(ERR_FAILED == task_accept_group(head, hs, (uint16_t)nnet), "accept group can be set only once");
    ASSERTAB(ERR_OK == task_listen(head, PACK_NONE, NULL, "127.0.0.1", ACP_GROUP_PORT, &id, NETEV_ACCEPT),
             "group listen failed");
    task_ungrab(head);
    FREE(hs);
    coro_sleep(task, 50);// listen 落地是异步的
    for (i = 0; i < 2 * nnet; i++) {
        ASSERTAB(ERR_OK == coro_connect(task, PACK_NONE, NULL, "127.0.0.1", ACP_GROUP_PORT, 0, NULL, &sk),
                 "connect group listener failed");
        rt = coro_send(task, &sk, "group", 5, &rlens, 1);
        ASSERTAB(NULL != rt && 5 == rlens && 0 == memcmp(rt, "group", 5), "group echo failed");
        ev_close(&task->loader->netev, &sk);
    }
    ev_unlisten(&task->loader->netev, id);
    ASSERTAB(2 * nnet == (uint32_t)ATOMIC_GET(&_member_accepted), "every accepted connection reaches a member");
    ASSERTAB(0 == ATOMIC_GET(&_member_bad), "member got a connection of another thread");
}
static void _sw_recv(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client,
                     uint8_t slice, void *data, size_t size) {
    (void)client; (void)slice;
    if (PACK_CUSTZ_FIXED == pktype && 1 == size && 'a' == *(char *)data) {
        ev_ud_pktype(&task->loader->netev, sk, PACK_NONE);
        ATOMIC_ADD(&_sw_switched, 1);
    } else if (PACK_NONE == pktype && 4 == size && 0 == memcmp(data, "rest", 4)) {
        ATOMIC_ADD(&_sw_rest, 1);
    } else {
        ATOMIC_ADD(&_sw_bad, 1);
    }
}
static void _sw_startup(task_ctx *task) {
    task_recved(task, _sw_recv);
}
// 收包当场处理的包里切了协议：同一次读到的后续字节得按新协议解(用接收组保证连接落在绑定线程上)；
// 之后组头退出，接收组的监听在 accept 时就拒收新连接
static void _check_group_switch(task_ctx *task) {
    uint32_t nnet = loader_nnet(task->loader);
    name_t *hs;
    task_ctx *head;
    sock_ctx sk;
    uint64_t id;
    size_t plen, rlens;
    char *pack, *buf;
    void *rt;
    int32_t i;
    MALLOC(hs, sizeof(name_t) * nnet);
    _group_new(task, "rpc_sw", _sw_startup, hs);
    coro_sleep(task, 100);// 等各组员跑完 STARTUP 闲下来，首包才会在收包当场处理
    head = task_grab(task->loader, hs[0]);
    ASSERTAB(NULL != head, "grab switch head failed");
    ASSERTAB(ERR_OK == task_accept_group(head, hs, (uint16_t)nnet), "set switch accept group failed");
    ASSERTAB(ERR_OK == task_listen(head, PACK_CUSTZ_FIXED, NULL, "127.0.0.1", EVTASK_SW_PORT, &id, 0),
             "switch listen failed");
    task_ungrab(head);
    coro_sleep(task, 50);// listen 落地是异步的
    ASSERTAB(ERR_OK == coro_connect(task, PACK_NONE, NULL, "127.0.0.1", EVTASK_SW_PORT, 0, NULL, &sk),
             "connect switch listener failed");
    pack = custz_pack(PACK_CUSTZ_FIXED, "a", 1, &plen);
    MALLOC(buf, plen + 4);
    memcpy(buf, pack, plen);
    memcpy(buf + plen, "rest", 4);
    FREE(pack);
    ev_send(&task->loader->netev, &sk, buf, plen + 4, 0);// 一次发出，对端多半一次读到
    for (i = 0; i < 100 && 0 == ATOMIC_GET(&_sw_rest) && 0 == ATOMIC_GET(&_sw_bad); i++) {
        coro_sleep(task, 10);
    }
    ev_close(&task->loader->netev, &sk);
    ASSERTAB(1 == ATOMIC_GET(&_sw_switched) && 1 == ATOMIC_GET(&_sw_rest) && 0 == ATOMIC_GET(&_sw_bad),
             "bytes after a protocol switch must be parsed by the new protocol");
    head = task_grab(task->loader, hs[0]);
    ASSERTAB(NULL != head, "grab switch head failed");
    task_close(head);
    task_ungrab(head);
    for (i = 0; i < 100 && NULL != (head = task_grab(task->loader, hs[0])); i++) {
        task_ungrab(head);
        coro_sleep(task, 10);
    }
    ASSERTAB(NULL == head, "switch head never exited");
    ASSERTAB(ERR_OK == coro_connect(task, PACK_NONE, NULL, "127.0.0.1", EVTASK_SW_PORT, 0, NULL, &sk),
             "connect switch listener failed");// 内核替监听口完成了握手，拒收发生在 accept 回调里
    rt = coro_send(task, &sk, "x", 1, &rlens, 1);
    ev_close(&task->loader->netev, &sk);
    ev_unlisten(&task->loader->netev, id);
    FREE(hs);
    ASSERTAB(NULL == rt && 0 == ATOMIC_GET(&_sw_bad), "group listener must reject connections once its head exited");
}
static void _ws_hs(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client,
                   int32_t erro, void *data, size_t lens) {
    (void)task; (void)sk; (void)pktype; (void)client; (void)data; (void)lens;
    if (ERR_OK == erro) {
        ATOMIC_ADD(&_ws_handshaked, 1);
    }
}
// 两次发送依次应收到：整帧，然后分片首片、末片、整帧。第几帧的分片标记对不上就是乱序；
// 同一连接的回调都在一条 net 线程上串行跑，先读序号再加一不会撕裂
static void _ws_recv(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client,
                     uint8_t slice, void *data, size_t size) {
    static const uint8_t order[] = { 0, PROT_SLICE_START, PROT_SLICE_END, 0 };
    uint32_t n = (uint32_t)ATOMIC_GET(&_ws_recved);
    (void)task; (void)sk; (void)pktype; (void)client; (void)data; (void)size;
    if (0 == ATOMIC_GET(&_ws_handshaked) || n >= sizeof(order) || order[n] != slice) {
        ATOMIC_ADD(&_ws_disorder, 1);
    }
    ATOMIC_ADD(&_ws_recved, 1);
}
static void _ws_startup(task_ctx *task) {
    task_handshaked(task, _ws_hs);
    task_recved(task, _ws_recv);
}
// 握手请求与首个数据帧同一次到达绑定 task：握手结果在嵌套窗口里入队，随后的数据帧
// 不能抢在它前面在收包当场被处理(用接收组保证连接落在绑定线程上，才走得到当场处理)。
// 第二次发送的分片只入队、关窗才激活，随后的整帧抢得到调度权，只有队列判空拦得住它跑到分片前面
static void _check_inline_order(task_ctx *task) {
    static const char req[] = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    uint32_t nnet = loader_nnet(task->loader);
    size_t rlens = sizeof(req) - 1;
    size_t flen;
    name_t *hs;
    task_ctx *head;
    sock_ctx sk;
    uint64_t id;
    char *frame, *buf;
    binary_ctx bw;
    int32_t i;
    MALLOC(hs, sizeof(name_t) * nnet);
    _group_new(task, "rpc_ws", _ws_startup, hs);
    coro_sleep(task, 100);// 等各组员跑完 STARTUP 闲下来，数据帧才够得着当场处理那条路
    head = task_grab(task->loader, hs[0]);
    ASSERTAB(NULL != head, "grab ws head failed");
    ASSERTAB(ERR_OK == task_accept_group(head, hs, (uint16_t)nnet), "set ws accept group failed");
    ASSERTAB(ERR_OK == task_listen(head, PACK_WEBSOCK, NULL, "127.0.0.1", EVTASK_WS_PORT, &id, 0), "ws listen failed");
    task_ungrab(head);
    FREE(hs);
    coro_sleep(task, 50);// listen 落地是异步的
    ASSERTAB(ERR_OK == coro_connect(task, PACK_NONE, NULL, "127.0.0.1", EVTASK_WS_PORT, 0, NULL, &sk),
             "connect ws task failed");
    frame = websock_pack_text(1, 1, (void *)"hi", 2, &flen);
    MALLOC(buf, rlens + flen);
    memcpy(buf, req, rlens);
    memcpy(buf + rlens, frame, flen);
    FREE(frame);
    ev_send(&task->loader->netev, &sk, buf, rlens + flen, 0);// 一次发出，对端多半一次读到
    for (i = 0; i < 100 && 0 == ATOMIC_GET(&_ws_recved); i++) {
        coro_sleep(task, 10);
    }
    // 组员此时已闲下来：分片首片、末片与一个整帧拼成一次发送
    binary_init_write(&bw, 0, 0);
    frame = websock_pack_text(1, 0, (void *)"a", 1, &flen);
    binary_set_binary(&bw, frame, flen);
    FREE(frame);
    frame = websock_pack_continua(1, 1, (void *)"b", 1, &flen);
    binary_set_binary(&bw, frame, flen);
    FREE(frame);
    frame = websock_pack_text(1, 1, (void *)"c", 1, &flen);
    binary_set_binary(&bw, frame, flen);
    FREE(frame);
    ev_send(&task->loader->netev, &sk, bw.data, bw.offset, 0);// 一次发出，对端多半一次读到
    for (i = 0; i < 100 && ATOMIC_GET(&_ws_recved) < 4; i++) {
        coro_sleep(task, 10);
    }
    ev_close(&task->loader->netev, &sk);
    ev_unlisten(&task->loader->netev, id);
    ASSERTAB(1 == ATOMIC_GET(&_ws_handshaked) && 4 == ATOMIC_GET(&_ws_recved), "ws handshake or frame lost");
    ASSERTAB(0 == ATOMIC_GET(&_ws_disorder), "frame overtook the handshake result or the slices queued before it");
}
// 转交用例的组员：首包载荷是要转去的句柄，收包当场(net 线程上 ev_ud_handle 立即生效)把连接转走
static void _ho_recv(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client,
                     uint8_t slice, void *data, size_t size) {
    name_t to;
    (void)client; (void)slice;
    if (PACK_CUSTZ_FIXED == pktype && sizeof(to) == size) {
        memcpy(&to, data, sizeof(to));
        ev_ud_handle(&task->loader->netev, sk, to);
        ATOMIC_ADD(&_ho_switched, 1);
    } else {
        ATOMIC_ADD(&_ho_bad, 1);
    }
}
static void _ho_startup(task_ctx *task) {
    task_recved(task, _ho_recv);
}
// 转交用例的新目标：只该收到首包之后的 "rest"
static void _ho_other_recv(task_ctx *task, sock_ctx *sk, subtype_t pktype, uint8_t client,
                           uint8_t slice, void *data, size_t size) {
    (void)task; (void)sk; (void)client; (void)slice;
    if (PACK_CUSTZ_FIXED == pktype && 4 == size && 0 == memcmp(data, "rest", 4)) {
        ATOMIC_ADD(&_ho_rest, 1);
    } else {
        ATOMIC_ADD(&_ho_bad, 1);
    }
}
static void _ho_other_startup(task_ctx *task) {
    task_recved(task, _ho_other_recv);
}
// 两个 custz 包拼成一次发送：首包载荷是要转去的句柄，次包是 "rest"
static char *_ho_frames(name_t to, size_t *lens) {
    size_t l1, l2;
    char *p1 = custz_pack(PACK_CUSTZ_FIXED, &to, sizeof(to), &l1);
    char *p2 = custz_pack(PACK_CUSTZ_FIXED, "rest", 4, &l2);
    char *buf;
    MALLOC(buf, l1 + l2);
    memcpy(buf, p1, l1);
    memcpy(buf + l1, p2, l2);
    FREE(p1);
    FREE(p2);
    *lens = l1 + l2;
    return buf;
}
// 组员收包当场把连接转给别的 task：同一次读到的后续包投给新目标；新目标不在就关连接，
// 后续包谁也不给(包有没有放由退出时的内存检查兜)
static void _check_group_handoff(task_ctx *task) {
    uint32_t nnet = loader_nnet(task->loader);
    name_t *hs;
    task_ctx *head, *other;
    name_t to;
    sock_ctx sk;
    uint64_t id, t0;
    size_t blen, rlens;
    char *buf;
    void *rt;
    int32_t i;
    MALLOC(hs, sizeof(name_t) * nnet);
    _group_new(task, "rpc_ho", _ho_startup, hs);
    other = task_new(task->loader, "rpc_ho_other", 0, NULL, NULL, NULL);
    to = other->handle;
    if (ERR_OK != task_register(other, _ho_other_startup, NULL)) {
        task_free(other);
        ASSERTAB(0, "register handoff target failed");
    }
    coro_sleep(task, 100);// 等各组员跑完 STARTUP 闲下来，首包才会在收包当场处理
    head = task_grab(task->loader, hs[0]);
    ASSERTAB(NULL != head, "grab handoff head failed");
    ASSERTAB(ERR_OK == task_accept_group(head, hs, (uint16_t)nnet), "set handoff accept group failed");
    ASSERTAB(ERR_OK == task_listen(head, PACK_CUSTZ_FIXED, NULL, "127.0.0.1", EVTASK_HO_PORT, &id, 0),
             "handoff listen failed");
    task_ungrab(head);
    FREE(hs);
    coro_sleep(task, 50);// listen 落地是异步的
    // 转给普通 task：首包归组员，次包归新目标
    ASSERTAB(ERR_OK == coro_connect(task, PACK_NONE, NULL, "127.0.0.1", EVTASK_HO_PORT, 0, NULL, &sk),
             "connect handoff listener failed");
    buf = _ho_frames(to, &blen);
    ev_send(&task->loader->netev, &sk, buf, blen, 0);// 一次发出，对端多半一次读到
    for (i = 0; i < 100 && 0 == ATOMIC_GET(&_ho_rest) && 0 == ATOMIC_GET(&_ho_bad); i++) {
        coro_sleep(task, 10);
    }
    ev_close(&task->loader->netev, &sk);
    ASSERTAB(1 == ATOMIC_GET(&_ho_switched) && 1 == ATOMIC_GET(&_ho_rest) && 0 == ATOMIC_GET(&_ho_bad),
             "packets after a handoff must reach the new handler");
    // 转给从没注册过的句柄：框架当场关连接，客户端在读超时之前就看到断开
    ASSERTAB(ERR_OK == coro_connect(task, PACK_NONE, NULL, "127.0.0.1", EVTASK_HO_PORT, 0, NULL, &sk),
             "connect handoff listener failed");
    buf = _ho_frames(createid(), &blen);
    t0 = timer_cur_ms(&task->loader->timer);
    rt = coro_send(task, &sk, buf, blen, &rlens, 0);
    t0 = timer_cur_ms(&task->loader->timer) - t0;
    ev_close(&task->loader->netev, &sk);
    ev_unlisten(&task->loader->netev, id);
    ASSERTAB(NULL == rt && t0 < task_get_netread_timeout(task), "handoff to a gone handler must close the connection");
    ASSERTAB(2 == ATOMIC_GET(&_ho_switched) && 1 == ATOMIC_GET(&_ho_rest) && 0 == ATOMIC_GET(&_ho_bad),
             "nothing may be delivered after a handoff to a gone handler");
}
// 绑在 net 线程 0 上的 task：普通 task 的 startup、请求、收包、定时器都在 net 线程 0 上跑；
// 协程 task 挂起等 worker 上的本 task 回包，醒来仍在 net 线程 0；两边照常互发消息
static void _check_evtask(void *owner, void *arg) {
    task_ctx *task = owner;
    task_ctx *dst;
    name_t handle, cohandle;
    sock_ctx sk;
    void *rt;
    size_t rlens;
    int32_t erro, i;
    (void)arg;
    dst = task_new(task->loader, "rpc_evtask", 0, NULL, NULL, NULL);
    handle = dst->handle;
    ASSERTAB(INVALID_INDEX == task_net_index(dst), "unbound task net index must be INVALID_INDEX");
    ASSERTAB(ERR_FAILED == task_bind_net(dst, (int32_t)loader_nnet(task->loader)), "out of range net index must fail");
    ASSERTAB(ERR_FAILED == task_bind_net(dst, INVALID_INDEX - 1), "net index below INVALID_INDEX must fail");
    ASSERTAB(ERR_OK == task_bind_net(dst, 0) && 0 == task_net_index(dst), "bind net 0 failed");
    if (ERR_OK != task_register(dst, _evt_startup, NULL)) {
        task_free(dst);
        ASSERTAB(0, "register evtask failed");
    }
    for (i = 0; i < 100 && 0 == ATOMIC_GET(&_evt_started); i++) {
        coro_sleep(task, 10);
    }
    ASSERTAB(1 == ATOMIC_GET(&_evt_started), "evtask startup never ran");
    for (i = 0; i < 100 && 0 == ATOMIC_GET(&_evt_timeouted); i++) {
        coro_sleep(task, 10);
    }
    ASSERTAB(1 == ATOMIC_GET(&_evt_timeouted), "evtask timeout never fired");
    dst = task_grab(task->loader, handle);
    ASSERTAB(NULL != dst, "grab evtask failed");
    rt = coro_request(dst, task, EVTASK_REQ, "x", 1, 1, &erro, &rlens);
    task_ungrab(dst);
    ASSERTAB(NULL != rt && ERR_OK == erro && sizeof(int32_t) == rlens && 0 == *(int32_t *)rt,
             "evtask request must run on net thread 0");
    ASSERTAB(ERR_OK == coro_connect(task, PACK_NONE, NULL, "127.0.0.1", EVTASK_PORT, 0, NULL, &sk),
             "connect evtask failed");
    rt = coro_send(task, &sk, "evtask", 6, &rlens, 1);
    ASSERTAB(NULL != rt && 6 == rlens && 0 == memcmp(rt, "evtask", 6), "evtask echo failed");
    ev_close(&task->loader->netev, &sk);
    // 协程 task 由 coro_task_register 代为注册，只能注册后再绑，startup 那一轮在 worker 上
    dst = coro_task_register(task->loader, "rpc_evtask_co", 0, _evtco_startup, NULL, NULL, NULL);
    ASSERTAB(NULL != dst, "register evtask coro failed");
    cohandle = dst->handle;
    ASSERTAB(ERR_OK == task_bind_net(dst, 0), "bind coro task failed");
    // 绑定前已排进 worker 那一轮的消息仍在 worker 上跑：等 startup 那一轮开跑了再发请求
    for (i = 0; i < 100 && 0 == ATOMIC_GET(&_evtco_started); i++) {
        coro_sleep(task, 10);
    }
    ASSERTAB(1 == ATOMIC_GET(&_evtco_started), "evtask coro startup never ran");
    dst = task_grab(task->loader, cohandle);
    ASSERTAB(NULL != dst, "grab evtask coro failed");
    rt = coro_request(dst, task, EVTASK_REQ_CO, "x", 1, 1, &erro, &rlens);
    task_ungrab(dst);
    ASSERTAB(NULL != rt && ERR_OK == erro && sizeof(int32_t) == rlens && 0 == *(int32_t *)rt,
             "evtask coro must resume on net thread 0");
    // 绑定 task 主动发起的连接落在它绑的线程上
    dst = task_grab(task->loader, cohandle);
    ASSERTAB(NULL != dst, "grab evtask coro failed");
    rt = coro_request(dst, task, EVTASK_REQ_CONN, "x", 1, 1, &erro, &rlens);
    task_ungrab(dst);
    ASSERTAB(NULL != rt && ERR_OK == erro && sizeof(int32_t) == rlens && 0 == *(int32_t *)rt,
             "connection of a bound task must land on its net thread");
    ASSERTAB(0 == ATOMIC_GET(&_evt_bad), "evtask callback ran off its net thread");
    _check_accept_group(task);
    _check_inline_order(task);
    _check_group_switch(task);
    _check_group_handoff(task);
    LOG_INFO("rpc evtask checks passed.");// 失败都是 ASSERTAB 当场退出，跑完要留个记号，不然看不出跑没跑到这
}
static void _startup(task_ctx *task) {
    task_requested(task, _requested);
    _check_post_to(task);
    coro_fork(coro_task_co(task), _check_emit_nested, NULL);// 要等，别卡住本 task 的请求处理
    coro_fork(coro_task_co(task), _check_evtask, NULL);
}
void task_rpc_start(loader_ctx *loader, const char *name, int32_t pt) {
    _prt = pt;
    coro_task_register(loader, name, 0, _startup, NULL, NULL, NULL);
}
