#include "test_base.h"
#include "test_containers.h"
#include "test_hashset.h"
#include "test_path_trie.h"
#include "test_crypt.h"
#include "test_utils.h"
#include "test_seri.h"
#include "test_thread.h"
#include "test_stm.h"
#include "test_protocol.h"
#include "test_bson.h"
#include "test_mqtt_pack.h"
#include "test_pgsql_pack.h"
#include "test_mysql_pack.h"
#include "test_mongo_pack.h"
#include "test_mysql_parse.h"
#include "test_pgsql_parse.h"
#include "bench_lbytecache.h"
#include "bench_rwlock.h"
#include "bench_mpq.h"
#include "bench_evcmd.h"
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
#include "task_coro_extra.h"
#include "task_fork.h"
#include "task_serial.h"
#include "task_multicast.h"
#include "task_udp_multicast.h"
#include "task_multi_call.h"
#include "task_dc_client.h"
#include "task_sc_client.h"
#include "task_listen_churn.h"
#include "task_listen_unlisten_race.h"
#include "task_close_graceful.h"
#include "task_sendbuf_warn.h"
#include "task_priority.h"
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
    /* 基础初始化 */
#if defined(OS_WIN)
    timeBeginPeriod(1);
#endif
    sock_init();
    unlimit();
    srand((uint32_t)time(NULL));
    log_init(NULL, 0);
    bson_globle_init();
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
    //event 命令通道:pipe 直接 vs queue+spinlock(pipe 信号)
    bench_evcmd();
    LOG_INFO("*******************benchmark end*******************");
#endif
    /* ── 层 1：纯内存单元测试套件 ── */
    CuString *output = CuStringNew();
    CuSuite  *suite  = CuSuiteNew();

    test_base(suite);        /* 内存宏、原子操作 */
    test_containers(suite);  /* mpq、hashmap、heap、queue、sarray */
    test_hashset(suite);     /* hashset(hashmap 包装) */
    test_path_trie(suite);   /* path_trie 分层路径前缀树 + 通配匹配 */
    test_crypt(suite);       /* base64、crc、digest、hmac、urlraw、xor */
    test_utils(suite);       /* pack/unpack、binary、buffer、sfid、hash_ring、netaddr */
    test_seri(suite);        /* seri 二进制序列化：基本类型 / int 各档 / 字符串 / 嵌套 table */
    test_thread(suite);      /* mutex、spinlock、rwlock、cond、thread */
    test_stm(suite);         /* stm 共享只读快照: new/update/grab_data/ungrab_data/free/ungrab 引用计数 */
    test_protocol(suite);    /* HTTP、Redis RESP、URL 解析、custz、DNS、WebSocket */
    test_bson(suite);        /* BSON 构建器、迭代器、find */
    test_mqtt_pack(suite);   /* MQTT 组包/解包往返 */
    test_pgsql_pack(suite);  /* PostgreSQL 组包 + bind */
    test_mysql_pack(suite);  /* MySQL 组包 + bind + lenenc */
    test_mongo_pack(suite);  /* MongoDB wire 组包 + parse */
    test_mysql_parse(suite); /* MySQL 解包 + reader 全接口 */
    test_pgsql_parse(suite); /* PostgreSQL 解包 + reader 全接口 */

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
    void *evssl_p12 = NULL;
    void *evssl_null = NULL;
    const char *ssl_server = "";
    const char *ssl_clientnull = "";
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
    evssl_p12 = evssl_p12_new(p12, "srey");
    evssl_register("p12", evssl_p12);
    evssl_null = evssl_new(NULL, NULL, NULL, SSL_FILETYPE_PEM);
    ssl_clientnull = "clientnull";
    evssl_register(ssl_clientnull, evssl_null);
#endif

    name_val_ctx testlist[] = {
        {"mqtt_test1", 0},
        {"mqtt_test2", 0},
        {"mqtt_test3", 0},
        {"mqtt_test4", 0},
        //{"smtp_test", 0},
        {"mysql_test", 0},
        {"pgsql_test", 0},
        {"redis_test", 0},
        {"mongo_test", 0},
        {"timeout_test1", 0},
        {"timeout_test2", 0},
        {"timeout_test3", 0},
        {"coro_extra", 0},
        {"fork_test", 0},
        {"serial_test", 0},
        {"multicast_test", 0},
        {"udp_multicast_test", 0},
        {"multi_call_test", 0},
        {"dc_client_test", 0},
        {"sc_client_test", 0},
        {"listen_churn", 0},
        {"unlisten_race", 0},
        {"close_graceful", 0},
        {"sendbuf_warn", 0},
        {"priority_test", 0},
        {"router_test", 0},
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
        0, _get_name_val(testlist, "mqtt_test1"));
    task_mqtt_client_start(g_loader, "mqtt_test2", MQTT_50, "127.0.0.1", 1884,
        0, _get_name_val(testlist, "mqtt_test2"));
    // mqtt_test3/4：连接进程内的 task_mqtt_server（端口 1883）
    task_mqtt_client_start(g_loader, "mqtt_test3", MQTT_311, "127.0.0.1", 1883,
        0, _get_name_val(testlist, "mqtt_test3"));
    task_mqtt_client_start(g_loader, "mqtt_test4", MQTT_50, "127.0.0.1", 1883,
        0, _get_name_val(testlist, "mqtt_test4"));
    //habor
    char hbkey[129];
    randstr(hbkey, 128);
    int32_t rtn = harbor_start(g_loader, "harbor", ssl_server, "0.0.0.0", (uint16_t)*(_get_name_val(portlist, "harbor")), hbkey);
    if (ERR_OK != rtn) {
        LOG_WARN("harbor start error.");
    }
    //datacenter 全局 KV + wait/唤醒,业务 task 之前注册;测试用 "datacenter"(与 srey/main.c 默认 dc_name 一致)
    const char *dc_name = "datacenter";
    if (ERR_OK != dc_start(g_loader, dc_name)) {
        LOG_WARN("dc_start error.");
    }
    //subcenter 订阅中心,业务 task 之前注册;测试用 "subcenter"(与 srey/main.c 默认 sc_name 一致)
    const char *sc_name = "subcenter";
    static path_rules sc_rules;
    path_rules_def(&sc_rules);
    if (ERR_OK != sc_start(g_loader, sc_name, &sc_rules)) {
        LOG_WARN("sc_start error.");
    }
    //smtp
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
    //模拟多路请求
    task_timeout_start(g_loader, "timeout_test1", rpcname, portlist,
         evssl_null, 1, hbkey, 0, _get_name_val(testlist, "timeout_test1"));
    task_timeout_start(g_loader, "timeout_test2", rpcname, portlist,
         evssl_null, 0, hbkey, 0, _get_name_val(testlist, "timeout_test2"));
    task_timeout_start(g_loader, "timeout_test3", rpcname, portlist,
         evssl_null, 0, hbkey, 0, _get_name_val(testlist, "timeout_test3"));
    //协程 API 边界/失败路径补充
    task_coro_extra_start(g_loader, "coro_extra",
        (uint16_t)*(_get_name_val(portlist, "http_sv")), rpcname,
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
    //datacenter set/get/wait/delete/list_keys 集成测试
    task_dc_client_start(g_loader, "dc_client_test", dc_name, _get_name_val(testlist, "dc_client_test"));
    //subcenter pub/sub/retained/shared/meta 集成测试
    task_sc_client_start(g_loader, "sc_client_test", sc_name, _get_name_val(testlist, "sc_client_test"));
    //Listener 动态生命周期回归：用专用端口 15010 避开其他服务
    task_listen_churn_start(g_loader, "listen_churn", 15010,
        _get_name_val(testlist, "listen_churn"));
    //SO_REUSEPORT + 多 watcher 下 ev_unlisten 与 in-flight accept 并发压力：端口 15011
    task_listen_unlisten_race_start(g_loader, "unlisten_race", 15011,
        _get_name_val(testlist, "unlisten_race"));
    //ev_close(immed=0) 优雅关闭数据完整性：端口 15015
    task_close_graceful_start(g_loader, "close_graceful", 15015,
        _get_name_val(testlist, "close_graceful"));
    //wb_size 字节告警 + 大数据完整性：端口 15016
    task_sendbuf_warn_start(g_loader, "sendbuf_warn", 15016,
        _get_name_val(testlist, "sendbuf_warn"));
    //task_set_priority / task_get_priority round-trip + clamp 单元测试
    task_priority_start(g_loader, "priority_test",
        _get_name_val(testlist, "priority_test"));
    //router: server + client 双 task 联调
    task_router_server_start(g_loader, "task_router_server",
        (uint16_t)*_get_name_val(portlist, "router_sv"));
    task_router_client_start(g_loader, "router_test",
        (uint16_t)*_get_name_val(portlist, "router_sv"),
        _get_name_val(testlist, "router_test"));

    //等 server task_listen 落地后再以子进程跑 Python 协议模糊
    MSLEEP(1000);
    static const struct { const char *script; const char *name; } pyitems[] = {
        {"test_http.py",  "python_http"},
        {"test_ws.py",    "python_ws"},
        {"test_mqtt.py",  "python_mqtt"},
        {"test_mixed.py", "python_mixed"},
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
        popen_waitexit(&pctx, 60000);
        while ((nread = popen_read(&pctx, outbuf, sizeof(outbuf) - 1)) > 0) {
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
    }
    hug_wait(&_hug);
    loader_free(g_loader);
    hug_free(&_hug);
    sock_clean();
#if defined(OS_WIN)
    timeEndPeriod(1);
#endif
    log_free();
    _memcheck();
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
