#include "test_base.h"
#include "test_containers.h"
#include "test_hashset.h"
#include "test_crypt.h"
#include "test_utils.h"
#include "test_seri.h"
#include "test_thread.h"
#include "test_stm.h"
#include "test_event.h"
#include "test_minicoro.h"
#include "test_protocol.h"
#include "test_bson.h"
#include "test_mqtt_pack.h"
#include "test_pgsql_pack.h"
#include "test_mysql_pack.h"
#include "test_mongo_pack.h"
#include "test_mysql_parse.h"
#include "test_pgsql_parse.h"
#include "test_advance.h"
#include "bench_lbytecache.h"
#include "bench_rwlock.h"
#include "bench_mpq.h"
#include "bench_evcmd.h"
#include "bench_hashmap.h"
#include "task_tcp_server.h"
#include "task_udp_server.h"
#include "task_rpc.h"
#include "task_timeout.h"
#include "task_auto_close.h"
#include "task_mqtt_server.h"
#include "task_mqtt_client.h"
#include "task_smtp.h"
#include "task_http_server.h"
#include "task_router.h"
#include "task_ws_server.h"
#include "task_mysql.h"
#include "task_pgsql.h"
#include "task_redis.h"
#include "task_mongo.h"
#include "task_dbrefcnt.h"
#include "task_coro_extra.h"
#include "task_fork.h"
#include "task_serial.h"
#include "task_multicast.h"
#include "task_udp_multicast.h"
#include "task_kcp.h"
#include "task_multi_call.h"
#include "task_debug.h"
#include "task_listen_churn.h"
#include "task_v6only.h"
#include "task_listen_unlisten_race.h"
#include "task_close_flush.h"
#include "task_sendbuf_warn.h"
#include "task_priority.h"
#include "task_selfpost.h"
#include "lib.h"
#if WITH_LUA && ENABLE_LUA_BYTECACHE
#include "lbind/lbytecache.h"
#endif

#ifdef OS_WIN
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "winmm.lib")
    #pragma comment(lib, "lib.lib")
#if WITH_LUA && ENABLE_LUA_BYTECACHE
    #pragma comment(lib, "lualib.lib")
#endif
    #if WITH_SSL
        #ifdef ARCH_X64
            #pragma comment(lib, "libcrypto_x64.lib")
            #pragma comment(lib, "libssl_x64.lib")
        #else
            #pragma comment(lib, "libcrypto.lib")
            #pragma comment(lib, "libssl.lib")
        #endif
    #endif
#endif

static hug_ctx _hug;           // 退出等待原语 (信号 handler 通过 sighandle data 拿到 &_hug 调 hug_wakeup)

// 信号处理回调: 通过 sighandle data 拿到 hug_ctx, 转发到 hug_wakeup 唤醒主线程
static void _on_sigcb(int32_t sig, void *arg) {
    (void)sig;
    hug_wakeup((hug_ctx *)arg);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    if (ERR_OK != hug_init(&_hug)) {
        return ERR_FAILED;
    }
    sighandle(_on_sigcb, &_hug);
    /* 基础初始化。与 srey/main.c 的 service_init 是同一套全局初始化，加减项要两处同步 */
#if defined(OS_WIN)
    timeBeginPeriod(1);
#endif
    sock_init();
    unlimit();
    srand((uint32_t)time(NULL));
    serviceid(1);/* 取 srey 的内置默认值，让 createid 的高 16 位与生产一致 */
    log_init(NULL, 0);
    bson_globle_init();
    locale_init();
    coro_desc_init(0);
    dns_set_ip("8.8.8.8");
    const char *local = procpath();
#if 0
    //对比测试
    LOG_INFO("*********************benchmark*********************");
    //lua普通加载与 lbytecache
#if WITH_LUA && ENABLE_LUA_BYTECACHE
    rwlock_distr_ctx lcklbc;
    rwlock_distr_init(&lcklbc, 2);
    lbc_init(&lcklbc);
    bench_lbytecache();
    lbc_free();
    rwlock_distr_free(&lcklbc);
#endif
    LOG_INFO("--------------------------------------------------");
    //rwlock_distr_ctx rwlock_ctx 
    bench_rwlock();
    LOG_INFO("--------------------------------------------------");
    //mpq 与 queue + spinlock
    bench_mpq();
    LOG_INFO("--------------------------------------------------");
    //event 命令通道:pipe 直写 vs queue+spin vs fsqu(linux 下即 mpq),含触发信号合并对比
    bench_evcmd();
    LOG_INFO("--------------------------------------------------");
    //hashmap set/get/delete 吞吐:默认初始容量(全程扩容) vs 预留容量(无扩容)
    bench_hashmap();
    LOG_INFO("*******************benchmark end*******************");
#endif
    /* ── 层 1：纯内存单元测试套件 ── */
    CuString *output = CuStringNew();
    CuSuite  *suite  = CuSuiteNew();

    test_base(suite);        /* 内存宏、原子操作 */
    test_containers(suite);  /* mpq、hashmap、heap、queue、sarray */
    test_hashset(suite);     /* hashset(hashmap 包装) */
    test_crypt(suite);       /* base64、crc、digest、hmac、urlraw、xor */
    test_utils(suite);       /* pack/unpack、binary、buffer、sfid、hash_ring、netaddr */
    test_seri(suite);        /* seri 二进制序列化：基本类型 / int 各档 / 字符串 / 嵌套 table；yyjson_helper */
    test_thread(suite);      /* mutex、spinlock、rwlock、cond、thread */
    test_stm(suite);         /* stm 共享只读快照: new/update/grab_data/ungrab_data/free/ungrab 引用计数 */
    test_event(suite);       /* event 层：关闭前冲刷、FIN 检出、close_type 三档 */
    test_minicoro(suite);    /* minicoro 本地补丁：栈底守卫字拦截越过栈底的写 */
    test_protocol(suite);    /* HTTP、Redis RESP、URL 解析、custz、DNS、WebSocket */
    test_bson(suite);        /* BSON 构建器、迭代器、find */
    test_mqtt_pack(suite);   /* MQTT 组包/解包往返 */
    test_pgsql_pack(suite);  /* PostgreSQL 组包 + bind */
    test_mysql_pack(suite);  /* MySQL 组包 + bind + lenenc */
    test_mongo_pack(suite);  /* MongoDB wire 组包 + parse */
    test_mysql_parse(suite); /* MySQL 解包 + reader 全接口 */
    test_pgsql_parse(suite); /* PostgreSQL 解包 + reader 全接口 */
    test_advance(suite);     /* advance 层：router 路径规范化 */

    CuSuiteRun(suite);
    CuSuiteSummary(suite, output);
    CuSuiteDetails(suite, output);
    printf("%s\n", output->buffer);

    int32_t unit_failed = suite->failCount;

    CuStringDelete(output);
    CuSuiteDelete(suite);

    g_loader = loader_init(0, 0, 0);
    char pandan[PATH_LENS];
    SNPRINTF(pandan, sizeof(pandan), "%s%s%s", local, PATH_SEPARATORSTR, "panda.png");
    void *evssl_server = NULL;
    void *evssl_null = NULL;
    void *evssl_hbcli = NULL;
    const char *ssl_server = "";
    const char *ssl_clientnull = "";
    const char *ssl_harbor = "";
#if WITH_SSL
    char ca[PATH_LENS];
    char svcrt[PATH_LENS];
    char svkey[PATH_LENS];
    char p12[PATH_LENS];
    SNPRINTF(ca, sizeof(ca), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "ca.crt");
    SNPRINTF(svcrt, sizeof(svcrt), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "server.crt");
    SNPRINTF(svkey, sizeof(svkey), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "server.key");
    SNPRINTF(p12, sizeof(p12), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "client.p12");
    evssl_server = evssl_new(ca, svcrt, svkey, SSL_FILETYPE_PEM);
    ssl_server = "server";
    evssl_register(ssl_server, evssl_server);
    void *evssl_p12 = evssl_p12_new(p12, "srey");
    evssl_register("p12", evssl_p12);
    evssl_null = evssl_new(NULL, NULL, NULL, SSL_FILETYPE_PEM);
    ssl_clientnull = "clientnull";
    evssl_register(ssl_clientnull, evssl_null);
    char clcrt[PATH_LENS];
    char clkey[PATH_LENS];
    SNPRINTF(clcrt, sizeof(clcrt), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "client.crt");
    SNPRINTF(clkey, sizeof(clkey), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "client.key");
    // harbor mTLS:server 端 PEER|FAIL 强制对端出证书,client 端带 client 证书验 server
    void *evssl_hbsrv = evssl_new(ca, svcrt, svkey, SSL_FILETYPE_PEM);
    evssl_verify(evssl_hbsrv, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    ssl_harbor = "hbserver";
    evssl_register(ssl_harbor, evssl_hbsrv);
    evssl_hbcli = evssl_new(ca, clcrt, clkey, SSL_FILETYPE_PEM);
    evssl_verify(evssl_hbcli, SSL_VERIFY_PEER, NULL);
    evssl_register("hbclient", evssl_hbcli);
#endif

    name_val_ctx testlist[] = {
        {"mqtt_test1", 0},
        {"mqtt_test2", 0},
        {"mqtt_test3", 0},
        {"mqtt_test4", 0},
        //{"smtp_test", 0},
        {"smtp_fake", 0},
        {"mysql_test", 0},
        {"pgsql_test", 0},
        {"redis_test", 0},
        {"mongo_test", 0},
        {"db_refcount", 0},
        {"timeout_test1", 0},
        {"timeout_test2", 0},
        {"timeout_test3", 0},
        {"coro_extra", 0},
        {"fork_test", 0},
        {"serial_test", 0},
        {"multicast_test", 0},
        {"udp_multicast_test", 0},
        {"multi_call_test", 0},
        {"debug_test", 0},
        {"listen_churn", 0},
        {"v6only_test", 0},
        {"unlisten_race", 0},
        {"close_flush", 0},
        {"sendbuf_warn", 0},
        {"priority_test", 0},
        {"selfpost_test", 0},
        {"router_test", 0},
        {"kcp_test", 0},
        {"kcp_test2", 0},
        {"kcp_test3", 0},
        {"kcp_test4", 0},
        {"python_http", 0},
        {"python_ws", 0},
        {"python_mqtt", 0},
        {"python_mixed", 0},

        {NULL, 0}
    };
    name_val_ctx portlist[] = {
        {"tcp_sv", 15000},
        {"udp_echo", 15001},
        {"http_sv", 15002},
        {"ws_sv", 15003},
        {"harbor", 15004},
        {"router_sv", 15005},
        {"router_idx_sv", 15006},
        {"debug_console", 15017},
        {"kcp_tcp", 15040},
        {"kcp_udp", 15041},

        {NULL, 0}
    };
    //time out 任务间通信 udp tcp 回显
    const char *rpcname = "task_rpc";
    task_rpc_start(g_loader, rpcname, 0);
    //tcp server
    task_tcp_erver_start(g_loader, "task_tcp_erver", *(_get_name_val(portlist, "tcp_sv")), evssl_server, rpcname, 0);
    //udp server
    task_udp_server_start(g_loader, "task_udp_server", *(_get_name_val(portlist, "udp_echo")), 0);
    //http server
    task_http_server_start(g_loader, "task_http_server", (uint16_t)*(_get_name_val(portlist, "http_sv")), 0);
    //websocket server
    task_ws_server_start(g_loader, "task_ws_server", (uint16_t)*(_get_name_val(portlist, "ws_sv")), 0);
    //mqtt 测试
    task_mqtt_server_start(g_loader, "task_mqtt_server", 1883, 0);
    // mqtt_test1/2：连接 docker EMQX（端口 1884）
    task_mqtt_client_start(g_loader, "mqtt_test1", MQTT_311, "127.0.0.1", 1884,
        0, 0, _get_name_val(testlist, "mqtt_test1"));
    task_mqtt_client_start(g_loader, "mqtt_test2", MQTT_50, "127.0.0.1", 1884,
        0, 0, _get_name_val(testlist, "mqtt_test2"));
    // mqtt_test3/4 连的是上面这个进程内 broker，故各自在协程里先等一会再连（见头文件说明）；
    // 1884 的 test1/test2 连的是外部 EMQX，不受本进程 task 启动顺序影响，故不等
    task_mqtt_client_start(g_loader, "mqtt_test3", MQTT_311, "127.0.0.1", 1883,
        500, 0, _get_name_val(testlist, "mqtt_test3"));
    task_mqtt_client_start(g_loader, "mqtt_test4", MQTT_50, "127.0.0.1", 1883,
        500, 0, _get_name_val(testlist, "mqtt_test4"));
    //habor
    int32_t rtn = harbor_start(g_loader, "harbor", ssl_harbor, "0.0.0.0", (uint16_t)*(_get_name_val(portlist, "harbor")));
    if (ERR_OK != rtn) {
        LOG_WARN("harbor start error.");
    }
    //smtp:假服务端 + 并发投递(不依赖外网账号)
    task_smtp_fake_start(g_loader, "smtp_fake", 12525, _get_name_val(testlist, "smtp_fake"));
    task_smtp_start(g_loader, "task_smtp", ssl_clientnull, "smtp.gmail.com", 465,
         "test@gmail.com", "12345678", "test@gmail.com",
         "test@163.com", "test@qq.com", pandan,
         1, _get_name_val(testlist, "smtp_test"));
    //数据库测试（依赖 docker-compose 启动的服务）
    task_mysql_start(g_loader, "mysql_test", "127.0.0.1", 3306,
         "admin", "12345678", "test", _get_name_val(testlist, "mysql_test"));
    task_pgsql_start(g_loader, "pgsql_test", "127.0.0.1", 5432,
         "admin", "12345678", "test", _get_name_val(testlist, "pgsql_test"));
    task_redis_start(g_loader, "redis_test", "127.0.0.1", 6379,
         NULL, _get_name_val(testlist, "redis_test"));
    task_mongo_start(g_loader, "mongo_test", "127.0.0.1", 27017,
         "admin", "12345678", "test", "admin", _get_name_val(testlist, "mongo_test"));
    //DB 绑定引用计数配对回归（连非法地址触发 ev_connect 同步失败，验证 acquire/udfree 配对不误 free）
    task_dbrefcnt_start(g_loader, "db_refcount", _get_name_val(testlist, "db_refcount"));
    //模拟多路请求
    task_timeout_start(g_loader, "timeout_test1", rpcname, portlist,
         evssl_null, evssl_hbcli, 1, 0, _get_name_val(testlist, "timeout_test1"));
    task_timeout_start(g_loader, "timeout_test2", rpcname, portlist,
         evssl_null, evssl_hbcli, 0, 0, _get_name_val(testlist, "timeout_test2"));
    task_timeout_start(g_loader, "timeout_test3", rpcname, portlist,
         evssl_null, evssl_hbcli, 0, 0, _get_name_val(testlist, "timeout_test3"));
    //协程 API 边界/失败路径补充
    task_coro_extra_start(g_loader, "coro_extra",
        (uint16_t)*(_get_name_val(portlist, "http_sv")),
        (uint16_t)*(_get_name_val(portlist, "udp_echo")), rpcname,
        _get_name_val(testlist, "coro_extra"));
    //coro_fork / coro_fork_wait 单元测试
    task_fork_start(g_loader, "fork_test", _get_name_val(testlist, "fork_test"));
    //coro_serial_new / coro_serial_call 单元测试
    task_serial_start(g_loader, "serial_test", _get_name_val(testlist, "serial_test"));
    //ev_send_multi 多播测试：端口 15012 避开其他服务
    task_multicast_start(g_loader, "multicast_test", 15012, _get_name_val(testlist, "multicast_test"));
    //ev_udp_join/leave/ttl/loop UDP 多播测试：端口 15014
    task_udp_multicast_start(g_loader, "udp_multicast_test", 15014, _get_name_val(testlist, "udp_multicast_test"));
    //task_multi_call 跨 task 广播测试：publisher "multi_call_test" + 5 个 "..._subN" subscriber
    task_multi_call_start(g_loader, "multi_call_test", _get_name_val(testlist, "multi_call_test"));
    //debug 控制台 + debug 命令链路集成测试：console 起在 15017
    if (ERR_OK != debug_console_start(g_loader, "debug_console", "127.0.0.1",
                                     (uint16_t)*(_get_name_val(portlist, "debug_console")))) {
        LOG_WARN("debug_console_start error.");
    }
    task_debug_start(g_loader, "debug_test",
        (uint16_t)*(_get_name_val(portlist, "debug_console")),
        _get_name_val(testlist, "debug_test"));
    //Listener 动态生命周期回归：用专用端口 15010 避开其他服务
    task_listen_churn_start(g_loader, "listen_churn", 15010,
        _get_name_val(testlist, "listen_churn"));
    //IPV6_V6ONLY 强制生效回归：端口 15013
    task_v6only_start(g_loader, "v6only_test", 15013,
        _get_name_val(testlist, "v6only_test"));
    //SO_REUSEPORT + 多 watcher 下 ev_unlisten 与 in-flight accept 并发压力：端口 15011
    task_listen_unlisten_race_start(g_loader, "unlisten_race", 15011,
        _get_name_val(testlist, "unlisten_race"));
    //ev_close 关闭前冲刷一次的契约（小包全达 / 大包截断但关得掉）：端口 15015
    task_close_flush_start(g_loader, "close_flush", 15015,
        _get_name_val(testlist, "close_flush"));
    //wb_size 字节告警 + 大数据完整性：端口 15016
    task_sendbuf_warn_start(g_loader, "sendbuf_warn", 15016,
        _get_name_val(testlist, "sendbuf_warn"));
    //task_set_priority / task_get_priority round-trip + clamp 单元测试
    task_priority_start(g_loader, "priority_test",
        _get_name_val(testlist, "priority_test"));
    //自投递不死锁: 自身 dispatch 内连续 task_call 自己, 条数远超 qumsg 容量
    task_selfpost_start(g_loader, "selfpost_test",
        _get_name_val(testlist, "selfpost_test"));
    //router: server + client 双 task 联调; 另起一个无流式路由的 server 压 chunked 无流式那条分支
    task_router_server_start(g_loader, "task_router_server",
        (uint16_t)*_get_name_val(portlist, "router_sv"));
    task_router_index_server_start(g_loader, "task_router_index_server",
        (uint16_t)*_get_name_val(portlist, "router_idx_sv"));
    task_router_client_start(g_loader, "router_test",
        (uint16_t)*_get_name_val(portlist, "router_sv"),
        (uint16_t)*_get_name_val(portlist, "router_idx_sv"),
        _get_name_val(testlist, "router_test"));
    //kcp: server 一个 task,client 侧四组场景(happy path/close 唤醒/同 session 并发 FIFO/kcp_synstart)各一个 task,update 业务层协程驱动
    task_kcp_server_start(g_loader, "kcp_server",
        (uint16_t)*_get_name_val(portlist, "kcp_tcp"),
        (uint16_t)*_get_name_val(portlist, "kcp_udp"));
    task_kcp_client_start(g_loader, "kcp_client",
        (uint16_t)*_get_name_val(portlist, "kcp_tcp"),
        (uint16_t)*_get_name_val(portlist, "kcp_udp"),
        _get_name_val(testlist, "kcp_test"));
    task_kcp_close_start(g_loader, "kcp_close",
        (uint16_t)*_get_name_val(portlist, "kcp_udp"),
        _get_name_val(testlist, "kcp_test2"));
    task_kcp_fifo_start(g_loader, "kcp_fifo",
        (uint16_t)*_get_name_val(portlist, "kcp_tcp"),
        (uint16_t)*_get_name_val(portlist, "kcp_udp"),
        _get_name_val(testlist, "kcp_test3"));
    task_kcp_synstart_start(g_loader, "kcp_synstart",
        (uint16_t)*_get_name_val(portlist, "kcp_udp"),
        _get_name_val(testlist, "kcp_test4"));

    //等各 server task 的 _startup 都派发完再以子进程跑 Python 协议模糊
    MSLEEP(1000);
    static const struct { const char *script; const char *name; } pyitems[] = {
        {"test_http.py",  "python_http"},
        {"test_ws.py",    "python_ws"},
        {"test_mqtt.py",  "python_mqtt"},
        {"test_mixed.py", "python_mixed"},
        // 不含 test_ssl_reneg.py: 它要连 15443 的 SSL 端口, 而 task_http_server 只起明文 15002。
        // 那条脚本只在 ./bin/srey 那侧跑(server_http.lua 起了 SSL 监听) —— 改 evssl / SSL 数据期
        // 的代码时按项目惯例跑的是 ./bin/test, 这条覆盖不到, 需要另跑一次 ./bin/srey
    };
    char pycmd[PATH_LENS];
    char outbuf[4096];
    int32_t nread;
    int32_t pycode;
    int32_t *pyslot;
    popen_ctx pctx;
    size_t pyi;
    for (pyi = 0; pyi < sizeof(pyitems) / sizeof(pyitems[0]); pyi++) {
        SNPRINTF(pycmd, sizeof(pycmd), "python3 %s%spy_assist%s%s",
            local, PATH_SEPARATORSTR, PATH_SEPARATORSTR, pyitems[pyi].script);
        PRINT("running %s", pycmd);
        if (ERR_OK != popen_startup(&pctx, pycmd, "r")) {
            LOG_WARN("popen %s failed.", pycmd);
            continue;
        }
        // 返回值不能丢: 脚本挂满 60 秒与正常跑完 3 秒, 日志里本来长得一模一样
        if (ERR_OK != popen_waitexit(&pctx, 60000)) {
            LOG_WARN("popen %s exceeded 60s budget.", pycmd);
        }
        while ((nread = popen_read(&pctx, outbuf, sizeof(outbuf) - 1, NULL)) > 0) {
            outbuf[nread] = '\0';
            printf("%s\n", outbuf);
        }
        pycode = popen_exitcode(&pctx);
        popen_free(&pctx);
        pyslot = _get_name_val(testlist, pyitems[pyi].name);
        if (NULL != pyslot && 0 == pycode) {
            *pyslot = 1;
        } else {
            LOG_WARN("%s exit code %d.", pyitems[pyi].name, pycode);
        }
        // 已经收到退出信号就别再起后面的脚本: 子进程自成进程组后收不到终端 SIGINT,
        // 挨个等满 60 秒预算会让 Ctrl+C 看起来几分钟没反应
        if (0 != ATOMIC_GET(&_hug.exitflag)) {
            LOG_WARN("exit signaled, skip remaining python assists.");
            break;
        }
    }
    hug_wait(&_hug);
    loader_free(g_loader);

    /* ── 会用光全局槽位的用例：排在集成阶段之后，别把分条计数提前关掉 ── */
    CuString *slotout = CuStringNew();
    CuSuite *slotsuite = CuSuiteNew();
    test_base_slots(slotsuite);
    CuSuiteRun(slotsuite);
    CuSuiteSummary(slotsuite, slotout);
    CuSuiteDetails(slotsuite, slotout);
    printf("%s\n", slotout->buffer);
    unit_failed += slotsuite->failCount;
    CuStringDelete(slotout);
    CuSuiteDelete(slotsuite);
    hug_free(&_hug);
    sock_clean();
#if defined(OS_WIN)
    timeEndPeriod(1);
#endif
    log_free();
    _memcheck();
    locale_free();
    PRINT("%s", "-----------test result-----------");
    uint32_t nclose = get_close_count();
    // auto_close 任务至少被触发一次才说明 _timeout_auto_close 路径有效
    if (0 == nclose) {
        unit_failed++;
        PRINT("auto close count: (0)");
    } else {
        PRINT("auto close count: (%u)", nclose);
    }
    int32_t optional;
    for (int32_t i = 0; ; i++) {
        if (NULL == testlist[i].name) {
            break;
        }
        // mqtt_test1/2 + mysql/pgsql/redis/mongo 依赖本机 docker，未启动允许失败
        optional = (0 == strcmp(testlist[i].name, "mqtt_test1")
                    || 0 == strcmp(testlist[i].name, "mqtt_test2")
                    || 0 == strcmp(testlist[i].name, "mysql_test")
                    || 0 == strcmp(testlist[i].name, "pgsql_test")
                    || 0 == strcmp(testlist[i].name, "redis_test")
                    || 0 == strcmp(testlist[i].name, "mongo_test"));
        if (testlist[i].val) {
            PRINT("%s: ok", testlist[i].name);
        } else if (optional) {
            PRINT("%s: - (network error)", testlist[i].name);
        } else {
            unit_failed++;
            PRINT("%s: x", testlist[i].name);
        }
    }
    return unit_failed;
}
