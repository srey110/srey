
#include "test_srey.h"
#include "lib.h"

/* =======================================================================
 * 公共同步原语 —— 主线程与工作线程之间的 mutex + cond 同步
 * ======================================================================= */

static mutex_ctx _mu;
static cond_ctx  _cond;
static volatile int _done;
static volatile int _pass;

static void _sync_init(void) {
    mutex_init(&_mu);
    cond_init(&_cond);
    _done = 0;
    _pass = 0;
}

static void _sync_free(void) {
    cond_free(&_cond);
    mutex_free(&_mu);
}

/* 在回调线程中调用：标记结果并唤醒主线程 */
static void _sync_signal(int ok) {
    mutex_lock(&_mu);
    _pass = ok;
    _done = 1;
    cond_signal(&_cond);
    mutex_unlock(&_mu);
}

/* 主线程中调用：最多等待 ms 毫秒。返回 1 表示收到信号且通过，0 表示超时或失败 */
static int _sync_wait(int ms) {
    mutex_lock(&_mu);
    while (!_done) {
        if (1 == cond_timedwait(&_cond, &_mu, ms)) {
            break; /* 超时 */
        }
    }
    int ok = _done && _pass;
    mutex_unlock(&_mu);
    return ok;
}

/* =======================================================================
 * 子测试 1：超时回调
 * 创建一个任务，在 startup 里注册 100ms 超时；超时触发后通知主线程。
 * ======================================================================= */

static void _to_timeout(task_ctx *task, uint64_t sess) {
    _sync_signal(1);
}

static void _to_startup(task_ctx *task) {
    task_timeout(task, 1, 100, _to_timeout); /* 100 ms */
}

static int _test_timeout(loader_ctx *loader) {
    _sync_init();

    task_ctx *t = task_new(loader, 2001, NULL, NULL, NULL);
    if (NULL == t) {
        _sync_free();
        printf("[test_srey] timeout:          FAIL (task_new)\n");
        return 0;
    }
    task_register(t, _to_startup, NULL);

    int ok = _sync_wait(3000); /* 最多等 3 秒 */

    task_close(t);
    _sync_free();
    printf("[test_srey] timeout:          %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* =======================================================================
 * 子测试 2：任务间请求/响应
 * 服务端任务（2002）收到请求后原样回复；
 * 客户端任务（2003）在 startup 里发请求，响应回调中验证内容并通知主线程。
 * ======================================================================= */

static name_t _rr_server = 2002;
static name_t _rr_client = 2003;

#define _RR_REQ_TYPE 1
#define _RR_MSG      "ping-pong"

/* 服务端：原样回复请求 */
static void _rr_server_request(task_ctx *task, uint8_t reqtype,
                               uint64_t sess, name_t src,
                               void *data, size_t size) {
    task_ctx *caller = task_grab(task->loader, src);
    if (NULL != caller) {
        task_response(caller, sess, ERR_OK, data, size, 1);
        task_ungrab(caller);
    }
}

/* 客户端：验证响应内容，通知主线程 */
static void _rr_client_response(task_ctx *task, uint64_t sess,
                                int32_t error, void *data, size_t size) {
    int ok = (ERR_OK == error)
          && (size == strlen(_RR_MSG))
          && (0 == memcmp(data, _RR_MSG, size));
    _sync_signal(ok);
}

/* 客户端：向服务端发送一次请求 */
static void _rr_client_startup(task_ctx *task) {
    task_ctx *sv = task_grab(task->loader, _rr_server);
    if (NULL != sv) {
        task_request(sv, task, _RR_REQ_TYPE, 1,
                     (void *)_RR_MSG, strlen(_RR_MSG), 1);
        task_ungrab(sv);
    } else {
        _sync_signal(0); /* 找不到服务端 */
    }
}

static int _test_request_response(loader_ctx *loader) {
    _sync_init();

    /* 先注册服务端，确保客户端 startup 时能 task_grab 到 */
    task_ctx *sv = task_new(loader, _rr_server, NULL, NULL, NULL);
    if (NULL == sv) {
        _sync_free();
        printf("[test_srey] request/response: FAIL (task_new server)\n");
        return 0;
    }
    task_requested(sv, _rr_server_request);
    task_register(sv, NULL, NULL);

    task_ctx *cl = task_new(loader, _rr_client, NULL, NULL, NULL);
    if (NULL == cl) {
        task_close(sv);
        _sync_free();
        printf("[test_srey] request/response: FAIL (task_new client)\n");
        return 0;
    }
    task_responsed(cl, _rr_client_response);
    task_register(cl, _rr_client_startup, NULL);

    int ok = _sync_wait(3000);

    task_close(cl);
    task_close(sv);
    _sync_free();
    printf("[test_srey] request/response: %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* =======================================================================
 * 子测试 3：TCP 回环收发（PACK_CUSTZ_FIXED）
 * 服务端任务（2004）监听 127.0.0.1:30199，收到数据后原样回显；
 * 客户端任务（2005）连接后发送消息，收到回显后验证内容并通知主线程。
 * ======================================================================= */

#define _TCP_PORT 30199
#define _TCP_MSG  "hello-srey-tcp"

/* 服务端：收到解包数据后重新打包并回显 */
static void _tcp_sv_recv(task_ctx *task, SOCKET fd, uint64_t skid,
                         uint8_t pktype, uint8_t client, uint8_t slice,
                         void *data, size_t size) {
    size_t pksize = 0;
    void *echo = custz_pack(PACK_CUSTZ_FIXED, data, size, &pksize);
    if (NULL != echo) {
        ev_send(&task->loader->netev, fd, skid, echo, pksize, 0);
    }
}

/* 服务端：注册监听 */
static void _tcp_sv_startup(task_ctx *task) {
    uint64_t id = 0;
    task_listen(task, PACK_CUSTZ_FIXED, NULL,
                "127.0.0.1", _TCP_PORT, &id, NETEV_NONE);
}

/* 客户端：连接成功后发送测试消息 */
static void _tcp_cl_connect(task_ctx *task, SOCKET fd, uint64_t skid,
                            uint8_t pktype, int32_t erro) {
    if (ERR_OK != erro) {
        _sync_signal(0); /* 连接失败 */
        return;
    }
    size_t pksize = 0;
    void *pkt = custz_pack(PACK_CUSTZ_FIXED,
                           (void *)_TCP_MSG, strlen(_TCP_MSG), &pksize);
    if (NULL != pkt) {
        ev_send(&task->loader->netev, fd, skid, pkt, pksize, 0);
    } else {
        _sync_signal(0);
    }
}

/* 客户端：收到回显后验证内容，关闭连接，通知主线程 */
static void _tcp_cl_recv(task_ctx *task, SOCKET fd, uint64_t skid,
                         uint8_t pktype, uint8_t client, uint8_t slice,
                         void *data, size_t size) {
    int ok = (size == strlen(_TCP_MSG))
          && (0 == memcmp(data, _TCP_MSG, size));
    ev_close(&task->loader->netev, fd, skid);
    _sync_signal(ok);
}

/* 客户端：发起连接 */
static void _tcp_cl_startup(task_ctx *task) {
    SOCKET fd = INVALID_SOCK;
    uint64_t skid = 0;
    int32_t rtn = task_connect(task, PACK_CUSTZ_FIXED, NULL,
                               "127.0.0.1", _TCP_PORT,
                               NETEV_NONE, NULL, &fd, &skid);
    if (ERR_OK != rtn) {
        _sync_signal(0); /* 发起连接失败 */
    }
}

static int _test_tcp_loopback(loader_ctx *loader) {
    _sync_init();

    /* 先注册服务端并等待其监听就绪 */
    task_ctx *sv = task_new(loader, 2004, NULL, NULL, NULL);
    if (NULL == sv) {
        _sync_free();
        printf("[test_srey] tcp loopback:     FAIL (task_new server)\n");
        return 0;
    }
    task_recved(sv, _tcp_sv_recv);
    task_register(sv, _tcp_sv_startup, NULL);

    /* 等待服务端 startup 完成并开始监听 */
    MSLEEP(200);

    /* 再注册客户端 */
    task_ctx *cl = task_new(loader, 2005, NULL, NULL, NULL);
    if (NULL == cl) {
        task_close(sv);
        _sync_free();
        printf("[test_srey] tcp loopback:     FAIL (task_new client)\n");
        return 0;
    }
    task_connected(cl, _tcp_cl_connect);
    task_recved(cl, _tcp_cl_recv);
    task_register(cl, _tcp_cl_startup, NULL);

    /* 等待回显验证完成，最多 5 秒 */
    int ok = _sync_wait(5000);

    task_close(cl);
    task_close(sv);
    _sync_free();
    printf("[test_srey] tcp loopback:     %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/* =======================================================================
 * 入口
 * ======================================================================= */

int test_srey(void) {
    printf("\n--- srey 集成测试 ---\n");

    loader_ctx *loader = loader_init(1, 2);
    if (NULL == loader) {
        printf("[test_srey] loader_init 失败\n");
        return 1;
    }
    g_loader = loader;

    int failed = 0;
    failed += !_test_timeout(loader);
    failed += !_test_request_response(loader);
    failed += !_test_tcp_loopback(loader);

    loader_free(loader);
    g_loader = NULL;

    printf("--- srey 集成测试完成 failed=%d ---\n\n", failed);
    return failed;
}
