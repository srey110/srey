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
#include "test_coro.h"
#include "test_protocol.h"
#include "test_bson.h"
#include "test_mqtt_pack.h"
#include "test_pgsql_pack.h"
#include "test_mysql_pack.h"
#include "test_mongo_pack.h"
#include "test_mysql_parse.h"
#include "test_pgsql_parse.h"
#include "test_advance.h"
#include "bench_mpq.h"
#include "bench_weights.h"
#include "bench_recvmmsg.h"
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
#include "task_acpstorm.h"
#include "task_v6only.h"
#include "task_listen_unlisten_race.h"
#include "task_close_flush.h"
#include "task_ssl_deadlock.h"
#include "task_ssl_keyupdate.h"
#include "task_sendbuf_warn.h"
#include "task_priority.h"
#include "task_selfpost.h"
#include "task_watch_closed.h"
#include "lib.h"

#ifdef OS_WIN
    // 各系统库由真正调用它的 lib 模块自己声明(同 srey/main.c)；test 不用 lualib 里的任何符号，不链它
    #pragma comment(lib, "lib.lib")
    #pragma comment(lib, "winmm.lib")// timeBeginPeriod
#endif

#define TEST_EXT_IP "127.0.0.1" // docker 起的外部服务（数据库、1884 的 MQTT broker）地址：VM 上跑时改成宿主机 IP，只改这一处

// 汇总 task 的参数：结果槽在 main 的栈上，主线程等到退出请求后才 loader_free，task 先于它失效
typedef struct test_reporter_arg {
    name_val_ctx *list;
    uint64_t start;// 开始等集成结果的时刻(_mono_ms)
}test_reporter_arg;

static hug_ctx _hug;// 退出等待原语 (信号 handler 通过 sighandle data 拿到 &_hug 调 hug_wakeup)
static atomic_t _exit_code = -1;// loader_exit 请求的退出码，只记第一次；-1 表示没人请求(信号退出)
static atomic_t _forcexit = 0;// 已请求过退出(信号或 loader_exit)后又来退出信号时置 1，看门狗见到就强退
static atomic_t _wdstate = 0;// 看门狗阶段：0 测试中，1 主线程已进 loader_free(开始计时)，2 loader_free 已返回
static timer_ctx _mono;// 单调计时，main 开头 timer_init；测试里的各种期限一律按它算，改系统时间不影响

// 自 main 开头起的毫秒数(单调钟)
static uint64_t _mono_ms(void) {
    return timer_elapsed_ms(&_mono);
}
// 信号处理回调: 通过 sighandle data 拿到 hug_ctx, 转发到 hug_wakeup 唤醒主线程。
// 已唤醒过说明这是再一次的退出请求：测试中只记标志，强退交给看门狗线程；loader_free 返回后看门狗已收工，
// 由这里直接 _exit(1)(这里只做信号上下文里安全的事)。
// abort() 也会进来：不当作退出请求(不记强退标志)，但照常唤醒主线程，最终由默认处理留 core(同 srey/main.c)
static void _on_sigcb(int32_t sig, void *arg) {
    hug_ctx *hug = (hug_ctx *)arg;
#ifdef OS_WIN
    (void)sig;
    int32_t isabort = 0;
#else
    int32_t isabort = SIGABRT == sig;
#endif
    if (!isabort
        && 0 != ATOMIC_GET(&hug->exitflag)) {
        // 看门狗已收工(loader_free 之后的收尾阶段)，没人再处理强退，当场退出(同 srey/main.c)
        if (2 == ATOMIC_GET(&_wdstate)) {
            _exit(1);
        }
        ATOMIC_SET(&_forcexit, 1);
    }
    hug_wakeup(hug);
}
// loader_exit 的回调(任意线程)：记下第一次请求的退出码，再同信号一样唤醒主线程。
// 这里只请求 0/1，-1 当"没人请求"的哨兵够用(srey/main.c 改用独立标志)
static void _on_exit(int32_t code, void *arg) {
    (void)ATOMIC_CAS(&_exit_code, -1, code);
    hug_wakeup((hug_ctx *)arg);
}
// thread_global_hooks 的全局 exit:与具体模块无关的线程级缓存在这里收,与 srey/main.c 同步(含 OpenSSL 那条的理由)
static void _thread_exit_hook(void *udata, void *assist) {
    (void)udata;
    (void)assist;
    buffer_thread_cleanup();
#if WITH_SSL && defined(OS_WIN)
    OPENSSL_thread_stop();
#endif
}

// 逐条列出套件里失败的用例; 成功的只进总计不占篇幅
static void _cusuite_fails(CuSuite *suite) {
    CuTest *tc;
    for (int32_t i = 0; i < suite->count; i++) {
        tc = suite->list[i];
        if (tc->failed) {
            PRINT("  x %s: %s", tc->name, tc->message);
        }
    }
}

// 依赖本机 docker 的用例（外部 EMQX 与四个数据库），没启动允许失败
static int32_t _test_optional(const char *name) {
    return 0 == strcmp(name, "mqtt_test1")
        || 0 == strcmp(name, "mqtt_test2")
        || 0 == strcmp(name, "mysql_test")
        || 0 == strcmp(name, "pgsql_test")
        || 0 == strcmp(name, "redis_test")
        || 0 == strcmp(name, "mongo_test");
}
// 还没到终态的结果槽个数。1 是通过、-3 是已失败；可选用例停在 0 说明压根没连上，也算终态；
// 其余（0 没走完、-2 正在连、-1 连上后没走完）都算未完成。print 非 0 时顺带逐个列出
static int32_t _test_pending(name_val_ctx *list, int32_t print) {
    int32_t n = 0;
    int32_t val;
    for (int32_t i = 0; NULL != list[i].name; i++) {
        val = (int32_t)ATOMIC_GET((atomic_t *)&list[i].val);// 各 task 在自己的线程里用 ATOMIC_SET 写
        if (1 == val
            || -3 == val
            || (0 == val && _test_optional(list[i].name))) {
            continue;
        }
        n++;
        if (print) {
            PRINT("  pending: %s (%d)", list[i].name, val);
        }
    }
    return n;
}
// 汇总 task 每 200ms 查一遍结果槽：全部到终态、或等满 300s(列出未完成项)就请求退出，主线程随即收尾。
// 退出码由 main 收尾时按结果槽统一算，这里给的只是个大概。SIGINT 仍可随时打断
static void _test_reporter_tick(task_ctx *task, uint64_t sess) {
    const uint64_t maxms = 300000;
    const uint32_t stepms = 200;
    test_reporter_arg *arg = (test_reporter_arg *)task->arg;
    (void)sess;
    if (task_isclosing(task)) {
        return;
    }
    if (0 == _test_pending(arg->list, 0)) {
        PRINT("%s", "all integration items finished, shutting down.");
        loader_exit(task->loader, 0);
        return;
    }
    if (_mono_ms() - arg->start >= maxms) {
        PRINT("integration wait timed out after %u s, shutting down with:", (uint32_t)(maxms / 1000));
        (void)_test_pending(arg->list, 1);
        loader_exit(task->loader, 1);
        return;
    }
    task_timeout(task, 0, stepms, _test_reporter_tick);
}
static void _test_reporter_startup(task_ctx *task) {
    _test_reporter_tick(task, 0);
}
// 起汇总 task 接着等集成结果；起不来就直接请求退出，免得主线程等不到人叫
static void _test_reporter_start(loader_ctx *loader, test_reporter_arg *arg) {
    task_ctx *task = task_new(loader, "test_reporter", 0, NULL, NULL, arg);
    if (ERR_OK != task_register(task, _test_reporter_startup, NULL)) {
        task_free(task);
        PRINT("%s", "test_reporter register failed, shutting down.");
        loader_exit(loader, 1);
    }
}
// 看门狗线程，CuTest 阶段起常驻、每 100ms 查一次：已请求退出后又来退出信号，或 loader_free 60 秒还没回来，
// 就列出未完成的用例后 _exit(1)，不出汇总。退不掉的 task 由 loader 关闭期按 CLOSING_WARN_MS 打的 close timeout 告警列出
static void _test_watchdog(void *arg) {
    const uint64_t freems = 60000;
    uint64_t deadline = 0;
    atomic_t state;
    for (;;) {
        state = ATOMIC_GET(&_wdstate);
        if (2 == state) {
            return;
        }
        if (0 != ATOMIC_GET(&_forcexit)) {
            PRINT("%s", "exit signaled again, force exit with:");
            break;
        }
        if (1 == state && 0 == deadline) {
            deadline = _mono_ms() + freems;
        } else if (0 != deadline && _mono_ms() >= deadline) {
            PRINT("loader_free not returned after %u s (stuck tasks: see 'close timeout' warnings), force exit with:",
                  (uint32_t)(freems / 1000));
            break;
        }
        MSLEEP(100);
    }
    (void)_test_pending((name_val_ctx *)arg, 1);
    fflush(stdout);// _exit 不冲 stdio 缓冲，输出重定向到文件或管道时不冲就丢
    _exit(1);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    timer_init(&_mono);
    if (ERR_OK != hug_init(&_hug)) {
        PRINT("hug_init failed.");
        return 1;// 与文件末尾归一后的退出码同口径；ERR_FAILED 是 -1，低 8 位得 255
    }
    sighandle(_on_sigcb, &_hug);
    /* 基础初始化。与 srey/main.c 的 service_init 是同一套全局初始化，加减项要两处同步 */
    thread_global_hooks(NULL, _thread_exit_hook);/* 须早于任何线程创建(log_init 起日志线程) */
#if defined(OS_WIN)
    timeBeginPeriod(1);
#endif
    locale_init();
    sock_init();
    srand((uint32_t)(time(NULL) ^ nowms() ^ GETPID()));// 只给 task_timeout 的 rand() 播种，srey 侧没有这项
    serviceid(1);/* 取 srey 的内置默认值，让 createid 的高 16 位与生产一致 */
    log_init(NULL, 0);
    unlimit();/* 须在 log_init 之后：NOFILE 上限不够的告警才打得出来，同 srey/main.c */
    bson_globle_init();
    coro_task_stack(0);
    dns_set_ip("8.8.8.8");
    const char *local = procpath();
#if 0
    //对比测试
    LOG_INFO("*********************benchmark*********************");
    //fsqu 三个后端(queue+spin / mpq / bbq)的入队出队
    bench_mpq();
    LOG_INFO("--------------------------------------------------");
    //loader worker weight 分档:不同 nworker 下的吞吐与各 task 完成离差
    bench_weights();
    LOG_INFO("--------------------------------------------------");
    //UDP 批量收(recvmmsg)对逐个收:定某平台该不该开 UDP_RECV_BATCH
    bench_recvmmsg();
    LOG_INFO("*******************benchmark end*******************");
    log_free();
    return 0;
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
        {"acpstorm", 0},
        {"v6only_test", 0},
        {"unlisten_race", 0},
        {"close_flush", 0},
#if WITH_SSL
        {"ssl_deadlock", 0},
        {"ssl_keyupdate", 0},
#endif
        {"sendbuf_warn", 0},
        {"priority_test", 0},
        {"selfpost_test", 0},
        {"watch_closed", 0},
        {"watch_closed_coro", 0},
        {"router_test", 0},
        {"kcp_test", 0},
        {"kcp_test2", 0},
        {"kcp_test3", 0},
        {"kcp_test4", 0},
        {"kcp_test5", 0},
#if KCP_TICK_HEAP
        {"kcp_test6", 0},
#endif
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
        {"acpstorm", 15100},// 每轮 +1,占到 15129
        {"kcp_tcp", 15040},
        {"kcp_udp", 15041},

        {NULL, 0}
    };
    // 再次退出信号与 loader_free 卡住的兜底，见 _test_watchdog；loader_free 返回后 join
    pthread_t watchdog = thread_creat(_test_watchdog, testlist);
    /* ── 层 1：纯内存单元测试套件 ── */
    CuSuite *suite = CuSuiteNew();

    test_base(suite);/* 内存宏、原子操作、字节序 pack/unpack、bytes / strconv、文件 / 时间 / strptime / 随机 / 进程、timer、once */
    test_containers(suite);/* mpq、bbq、spsc、fsqu、chan、hashmap、heap、queue、sarray、slist、rbtree */
    test_hashset(suite);/* hashset(hashmap 包装) */
    test_crypt(suite);/* base64、crc、digest、hmac、urlraw、xor、xxhash、crypt_memcmp */
    test_utils(suite);/* binary、buffer、sfid、uuid、hash_ring、netaddr、log、popen、tw */
    test_seri(suite);/* seri 二进制序列化：基本类型 / int 各档 / 字符串 / 嵌套 table；yyjson_helper */
    test_thread(suite);/* mutex、spinlock、rwlock、cond、thread、futex、waiter / spinner */
    test_stm(suite);/* stm 共享只读快照: new/update/grab_data/ungrab_data/free/ungrab 引用计数 */
    test_event(suite);/* event 层：关闭前冲刷、FIN 检出、close_type 三档 */
    test_minicoro(suite);/* minicoro 本地补丁：栈底守卫字拦截越过栈底的写 */
    test_coro(suite);/* 通用协程调度器：sess 五条规则、fork、serial、dump */
    test_protocol(suite);/* HTTP、Redis RESP、URL 解析、custz、DNS、WebSocket */
    test_bson(suite);/* BSON 构建器、迭代器、find */
    test_mqtt_pack(suite);/* MQTT 组包/解包往返 */
    test_pgsql_pack(suite);/* PostgreSQL 组包 + bind */
    test_mysql_pack(suite);/* MySQL 组包 + bind + lenenc */
    test_mongo_pack(suite);/* MongoDB wire 组包 + parse */
    test_mysql_parse(suite); /* MySQL 解包 + reader 全接口 */
    test_pgsql_parse(suite); /* PostgreSQL 解包 + reader 全接口 */
    test_advance(suite);/* advance 层：router 路径规范化 */

    CuSuiteRun(suite);
    // 套件留着不删，等槽位套件跑完在 test result 里合成一份总计。
    // 就地打结果的话中间隔着几百行集成日志，翻不回来

    g_loader = loader_init(0, 0, 0);
    loader_on_exit(g_loader, _on_exit, &_hug);
    char pandan[PATH_LENS];
    SNPRINTF(pandan, sizeof(pandan), "%s%s%s", local, PATH_SEPARATORSTR, "panda.png");
    void *evssl_server = NULL;
    void *evssl_null = NULL;
    void *evssl_hbcli = NULL;
    const char *ssl_clientnull = "";
    const char *ssl_harbor = "";
#if WITH_SSL
    const char *ssl_server;// 只在本块内用,放外面 WITH_SSL=0 时是 unused
    void *evssl_ku = NULL;// 同上
    char ca[PATH_LENS];
    char svcrt[PATH_LENS];
    char svkey[PATH_LENS];
    char p12[PATH_LENS];
    SNPRINTF(ca, sizeof(ca), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "ca.crt");
    SNPRINTF(svcrt, sizeof(svcrt), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "server.crt");
    SNPRINTF(svkey, sizeof(svkey), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "server.key");
    SNPRINTF(p12, sizeof(p12), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "client.p12");
    // 证书不入 git（.gitignore 忽略 *.crt/*.key），没跑过 bin/keys/create.sh 时这些全是 NULL。
    // 每步都印一行：register 里只 LOG_WARN，静默下去后面一串 SSL 用例会以"连不上"收场，
    // 看不出根因其实是证书没生成
    evssl_server = evssl_new(ca, svcrt, svkey, SSL_FILETYPE_PEM);
    if (NULL == evssl_server) {
        PRINT("evssl_new(server) failed, run bin/keys/create.sh first.");
    }
    ssl_server = "server";
    evssl_register(ssl_server, evssl_server);
    void *evssl_p12 = evssl_p12_new(p12, "srey");
    if (NULL == evssl_p12) {
        PRINT("evssl_p12_new failed, run bin/keys/create.sh first.");
    }
    evssl_register("p12", evssl_p12);
    evssl_null = evssl_new(NULL, NULL, NULL, SSL_FILETYPE_PEM);
    ssl_clientnull = "clientnull";
    evssl_register(ssl_clientnull, evssl_null);
    char clcrt[PATH_LENS];
    char clkey[PATH_LENS];
    SNPRINTF(clcrt, sizeof(clcrt), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "client.crt");
    SNPRINTF(clkey, sizeof(clkey), "%s%s%s%s%s", local, PATH_SEPARATORSTR, "keys", PATH_SEPARATORSTR, "client.key");
    // harbor mTLS:server 端 PEER|FAIL 强制对端出证书,client 端带 client 证书验 server。
    // evssl_verify 裸解引用,而 bin/keys 下的证书不入 git(.gitignore 忽略 *.crt/*.key),
    // 没跑过 create.sh 的新克隆拿到的就是 NULL
    void *evssl_hbsrv = evssl_new(ca, svcrt, svkey, SSL_FILETYPE_PEM);
    if (NULL != evssl_hbsrv) {
        evssl_verify(evssl_hbsrv, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    }
    ssl_harbor = "hbserver";
    evssl_register(ssl_harbor, evssl_hbsrv);
    evssl_hbcli = evssl_new(ca, clcrt, clkey, SSL_FILETYPE_PEM);
    if (NULL != evssl_hbcli) {
        evssl_verify(evssl_hbcli, SSL_VERIFY_PEER, NULL);
    }
    evssl_register("hbclient", evssl_hbcli);
    // ssl_keyupdate 专用:KeyUpdate 是 TLS1.3 独有的，钉死下限，
    // 否则协商到 1.2 时 evssl_keyupdate 直接失败，用例退化成普通大流量测试还报绿。
    // 钉不上就不起这个用例(evssl_ku 为 NULL 时 start 直接返回)，留在 pending 里报出来
    evssl_ku = evssl_new(ca, svcrt, svkey, SSL_FILETYPE_PEM);
    if (NULL != evssl_ku
        && ERR_OK != evssl_min_proto(evssl_ku, TLS1_3_VERSION)) {
        PRINT("evssl_min_proto(TLS1_3_VERSION) failed, ssl_keyupdate will not run.");
        evssl_free(evssl_ku);
        evssl_ku = NULL;
    }
    evssl_register("tls13", evssl_ku);
#endif

    //time out 任务间通信 udp tcp 回显
    const char *rpcname = "task_rpc";
    task_rpc_start(g_loader, rpcname, 0);
    //tcp server
    task_tcp_erver_start(g_loader, "task_tcp_erver", *(_get_name_val(portlist, "tcp_sv")), evssl_server, rpcname, 0);
    //udp server
    task_udp_server_start(g_loader, "task_udp_server", *(_get_name_val(portlist, "udp_echo")), PACK_NONE);
    //http server
    task_http_server_start(g_loader, "task_http_server", (uint16_t)*(_get_name_val(portlist, "http_sv")));
    //websocket server
    task_ws_server_start(g_loader, "task_ws_server", (uint16_t)*(_get_name_val(portlist, "ws_sv")));
    //mqtt 测试
    task_mqtt_server_start(g_loader, "task_mqtt_server", 1883, 0);
    // mqtt_test1/2：连接 docker EMQX（端口 1884）
    task_mqtt_client_start(g_loader, "mqtt_test1", MQTT_311, TEST_EXT_IP, 1884,
        0, 0, _get_name_val(testlist, "mqtt_test1"));
    task_mqtt_client_start(g_loader, "mqtt_test2", MQTT_50, TEST_EXT_IP, 1884,
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
    task_mysql_start(g_loader, "mysql_test", TEST_EXT_IP, 3306,
         "admin", "12345678", "test", _get_name_val(testlist, "mysql_test"));
    task_pgsql_start(g_loader, "pgsql_test", TEST_EXT_IP, 5432,
         "admin", "12345678", "test", _get_name_val(testlist, "pgsql_test"));
    task_redis_start(g_loader, "redis_test", TEST_EXT_IP, 6379,
         NULL, _get_name_val(testlist, "redis_test"));
    task_mongo_start(g_loader, "mongo_test", TEST_EXT_IP, 27017,
         "admin", "12345678", "test", "admin", _get_name_val(testlist, "mongo_test"));
    //DB 绑定引用计数配对回归（连非法地址触发 ev_connect 同步失败，验证 acquire/udfree 配对不误 free）
    task_dbrefcnt_start(g_loader, "db_refcount", _get_name_val(testlist, "db_refcount"));
    //模拟多路请求
    task_timeout_start(g_loader, "timeout_test1", rpcname, portlist,
         evssl_null, evssl_hbcli, 1, _get_name_val(testlist, "timeout_test1"));
    task_timeout_start(g_loader, "timeout_test2", rpcname, portlist,
         evssl_null, evssl_hbcli, 0, _get_name_val(testlist, "timeout_test2"));
    task_timeout_start(g_loader, "timeout_test3", rpcname, portlist,
         evssl_null, evssl_hbcli, 0, _get_name_val(testlist, "timeout_test3"));
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
    task_acpstorm_start(g_loader, "acpstorm",
        (uint16_t)*(_get_name_val(portlist, "acpstorm")),
        _get_name_val(testlist, "acpstorm"));
    //IPV6_V6ONLY 强制生效回归：端口 15013
    task_v6only_start(g_loader, "v6only_test", 15013,
        _get_name_val(testlist, "v6only_test"));
    //SO_REUSEPORT + 多 watcher 下 ev_unlisten 与 in-flight accept 并发压力：端口 15011
    task_listen_unlisten_race_start(g_loader, "unlisten_race", 15011,
        _get_name_val(testlist, "unlisten_race"));
    //ev_close 关闭前冲刷一次的契约（小包全达 / 大包截断但关得掉 / 解析出错就地关记 LOCAL）：端口 15015，解析出错那段用 15020
    task_close_flush_start(g_loader, "close_flush", 15015,
        _get_name_val(testlist, "close_flush"));
#if WITH_SSL
    //STATUS_WPEND_SSL 停读会不会让两端成环：SSL 环回互推 8MB，端口 15018
    task_ssl_deadlock_start(g_loader, "ssl_deadlock", 15018, evssl_server,
        _get_name_val(testlist, "ssl_deadlock"));
    //单向 8MB + 穿插 KeyUpdate：挂起写那条路能不能自己走完（够不到 KEYUPDATE_READ，见 .h），端口 15019
    task_ssl_keyupdate_start(g_loader, "ssl_keyupdate", 15019, evssl_ku,
        _get_name_val(testlist, "ssl_keyupdate"));
#endif
    //wb_size 字节告警 + 大数据完整性：端口 15016
    task_sendbuf_warn_start(g_loader, "sendbuf_warn", 15016,
        _get_name_val(testlist, "sendbuf_warn"));
    //task_set_priority / task_get_priority round-trip + clamp 单元测试
    task_priority_start(g_loader, "priority_test",
        _get_name_val(testlist, "priority_test"));
    //自投递不死锁: 自身 dispatch 内连续 task_call 自己, 条数远超 qumsg 容量
    task_selfpost_start(g_loader, "selfpost_test",
        _get_name_val(testlist, "selfpost_test"));
    //task_watch_closed: 两个观察者按登记顺序、先于业务关闭回调，NEVERCONN 不投(普通 task 与协程 task 各一)
    task_watch_closed_start(g_loader, (uint16_t)*(_get_name_val(portlist, "http_sv")),
        _get_name_val(testlist, "watch_closed"), _get_name_val(testlist, "watch_closed_coro"));
    //router: server + client 双 task 联调; 另起一个无流式路由的 server 压 chunked 无流式那条分支
    task_router_server_start(g_loader, "task_router_server",
        (uint16_t)*_get_name_val(portlist, "router_sv"));
    task_router_index_server_start(g_loader, "task_router_index_server",
        (uint16_t)*_get_name_val(portlist, "router_idx_sv"));
    task_router_client_start(g_loader, "router_test",
        (uint16_t)*_get_name_val(portlist, "router_sv"),
        (uint16_t)*_get_name_val(portlist, "router_idx_sv"),
        _get_name_val(testlist, "router_test"));
    //kcp: server 一个 task,client 侧六组场景(happy path/close 唤醒/同 session 并发 FIFO/kcp_synstart/空闲停表唤醒/死链拆会话)各一个 task,update 由 event 线程 tick 驱动;
    //死链只在 KCP_TICK_HEAP 为 1 时判,那组只在这时跑
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
    task_kcp_idle_start(g_loader, "kcp_idle",
        (uint16_t)*_get_name_val(portlist, "kcp_tcp"),
        (uint16_t)*_get_name_val(portlist, "kcp_udp"),
        _get_name_val(testlist, "kcp_test5"));
#if KCP_TICK_HEAP
    task_kcp_dead_start(g_loader, "kcp_dead",
        _get_name_val(testlist, "kcp_test6"));
#endif

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
    int32_t pyeof;
    int32_t *pyslot;
    uint64_t pydl;
    uint64_t pynow;
    popen_ctx pctx;
    size_t pyi;
    for (pyi = 0; pyi < sizeof(pyitems) / sizeof(pyitems[0]); pyi++) {
        // 已经收到退出信号就别再起脚本(含 CuTest 阶段就按下的): 子进程自成进程组后收不到终端 SIGINT,
        // 挨个等满 60 秒预算会让 Ctrl+C 看起来几分钟没反应
        if (0 != ATOMIC_GET(&_hug.exitflag)) {
            LOG_WARN("exit signaled, skip remaining python assists.");
            break;
        }
        // 路径加引号：exe 目录带空格时不加会被 shell 拆成两个参数
        SNPRINTF(pycmd, sizeof(pycmd), "python3 \"%s%spy_assist%s%s\"",
            local, PATH_SEPARATORSTR, PATH_SEPARATORSTR, pyitems[pyi].script);
        PRINT("running %s", pycmd);
        if (ERR_OK != popen_startup(&pctx, pycmd, "r")) {
            LOG_WARN("popen %s failed.", pycmd);
            pyslot = _get_name_val(testlist, pyitems[pyi].name);
            if (NULL != pyslot) {
                test_fail(pyslot);
            }
            continue;
        }
        // 边读边等：先等退出再读的话，输出超过管道缓冲时子进程卡在写上退不了，白等满 60 秒。
        // 必须看 eof 出参：返回 0 是"此刻没数据"，不是流末尾，按 EOF 处理会把输出截一半
        pyeof = 0;
        pydl = _mono_ms() + 60000;
        while (0 == pyeof && _mono_ms() < pydl) {
            nread = popen_read(&pctx, outbuf, sizeof(outbuf) - 1, &pyeof);
            if (ERR_FAILED == nread) {
                break;
            }
            if (nread > 0) {
                outbuf[nread] = '\0';
                printf("%s\n", outbuf);
            } else if (0 == pyeof) {
                MSLEEP(1);
            }
        }
        // 输出读完再等退出，预算是读和等合起来的 60 秒。返回值不能丢: 脚本挂满 60 秒与正常跑完 3 秒, 日志里本来长得一模一样
        pynow = _mono_ms();
        if (ERR_OK != popen_waitexit(&pctx, pynow < pydl ? (uint32_t)(pydl - pynow) : 0)) {
            LOG_WARN("popen %s exceeded 60s budget.", pycmd);
        }
        pycode = popen_exitcode(&pctx);
        popen_free(&pctx);
        pyslot = _get_name_val(testlist, pyitems[pyi].name);
        if (NULL != pyslot && 0 == pycode) {
            ATOMIC_SET((atomic_t *)pyslot, 1);
        } else {
            LOG_WARN("%s exit code %d.", pyitems[pyi].name, pycode);
            if (NULL != pyslot) {
                test_fail(pyslot);
            }
        }
    }
    test_reporter_arg reparg = { testlist, _mono_ms() };
    _test_reporter_start(g_loader, &reparg);
    hug_wait(&_hug);
    if (-1 == (int32_t)ATOMIC_GET(&_exit_code)) {
        PRINT("%s", "exit signaled, shutting down (signal again to force exit).");
    }
    ATOMIC_SET(&_wdstate, 1);
    loader_free(g_loader);
    ATOMIC_SET(&_wdstate, 2);
    thread_join(watchdog);

    /* ── 会用光全局槽位的用例：排在集成阶段之后，别把分条计数提前关掉 ── */
    CuSuite *slotsuite = CuSuiteNew();
    test_base_slots(slotsuite);
    CuSuiteRun(slotsuite);
    hug_free(&_hug);
    sock_clean();
#if defined(OS_WIN)
    timeEndPeriod(1);
#endif
    log_free();
    buffer_thread_cleanup();
    int64_t leak = _memcheck();
    locale_free();
    PRINT("%s", "-----------test result-----------");
    // CuSuite 走的是裸 malloc，不进 _memcheck 的账，留到内存检查之后再收也不会多报一笔
    int32_t cu_total = suite->count + slotsuite->count;
    int32_t unit_failed = suite->failCount + slotsuite->failCount;
    PRINT("cutest: total %d, passed %d, failed %d", cu_total, cu_total - unit_failed, unit_failed);
    _cusuite_fails(suite);
    _cusuite_fails(slotsuite);
    CuSuiteDelete(suite);
    CuSuiteDelete(slotsuite);
    // 泄漏与多释放都算失败。这一路以前只打印不计数，于是所有"漏没漏由收尾内存检查兜底"
    // 的用例都等于没有断言
    if (0 != leak) {
        unit_failed++;
    }
    uint32_t nclose = get_close_count();
    // 退出时的 CLOSING 广播自己就会记一笔，故 >= 2 才说明 task_close 真跑过
    PRINT("auto close count: %u", nclose);
    if (nclose < 2) {
        unit_failed++;
    }
    // 每次关闭都要分发 CLOSING，分发完必有一轮结束，所以回调次数不会少于关闭次数
    uint32_t nround = get_round_end_count();
    PRINT("round end count: %u", nround);
    if (nround < nclose) {
        unit_failed++;
    }
    int32_t optional;
    for (int32_t i = 0; ; i++) {
        if (NULL == testlist[i].name) {
            break;
        }
        // 可选用例只放行 val == 0（压根没连上），-3 是已失败、-1 是连上后没走完、-2 是收尾时还在连，一律计失败——
        // 前两者以前都是 0，于是这六条在 docker 起着的标准环境下永久不 gate 任何东西
        optional = _test_optional(testlist[i].name);
        if (1 == testlist[i].val) {
            PRINT("%s: ok", testlist[i].name);
        } else if (0 == testlist[i].val && optional) {
            PRINT("%s: - (network error)", testlist[i].name);
        } else {
            unit_failed++;
            PRINT("%s: x%s", testlist[i].name,
                  (-3 == testlist[i].val) ? " (failed)"
                  : ((-1 == testlist[i].val) ? " (connected, not finished)"
                  : ((-2 == testlist[i].val) ? " (still connecting)" : "")));
        }
    }
    // 归一成 0/1：退出码只取低 8 位，而失败数上限约 515，恰为 256 的倍数时会得 0；
    // 具体条数看上面的逐条输出
    return unit_failed ? 1 : 0;
}
