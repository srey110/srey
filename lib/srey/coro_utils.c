#include "srey/coro_utils.h"
#include "srey/coro.h"
#include "srey/task.h"
#include "srey/prots_wrap.h"
#include "protocol/prots.h"
#include "protocol/urlparse.h"
#include "protocol/dns.h"
#include "protocol/http.h"
#include "protocol/redis.h"
#include "protocol/mysql/mysql_parse.h"
#include "protocol/mysql/mysql_pack.h"
#include "serial/bson.h"
#include "utils/buffer.h"
#include "utils/utils.h"

static dns_ip *_dns_lookup_udp(task_ctx *task, const char *domain, int32_t ipv6, size_t *cnt, int32_t *nodata) {
    int32_t rtn;
    SOCKET fd;
    uint64_t skid;
    const char *dnsip = dns_get_ip();
    if (ERR_OK == is_ipv6(dnsip)) {
        rtn = task_udp(task, PACK_NONE, "::", 0, &fd, &skid);
    } else {
        rtn = task_udp(task, PACK_NONE, "0.0.0.0", 0, &fd, &skid);
    }
    if (ERR_OK != rtn) {
        return NULL;
    }
    coro_sync(task, fd, skid);
    char buf[ONEK];
    uint16_t id;
    size_t lens = dns_request_pack(buf, domain, ipv6, &id);
    if (0 == lens) {
        ev_close(&task->loader->netev, fd, skid, 1);
        return NULL;
    }
    void *resp = coro_sendto(task, fd, skid, dnsip, 53, buf, lens, &lens, 1);
    ev_close(&task->loader->netev, fd, skid, 1);
    if (NULL == resp) {
        return NULL;
    }
    return dns_parse_pack(resp, lens, cnt, id, nodata);
}
static dns_ip *_dns_lookup_tcp(task_ctx *task, const char *domain, int32_t ipv6, size_t *cnt) {
    SOCKET fd;
    uint64_t skid;
    const char *dnsip = dns_get_ip();
    if (ERR_OK != coro_connect(task, PACK_DNS, NULL, dnsip, 53, 0, NULL, &fd, &skid)) {
        return NULL;
    }
    char buf[ONEK];
    uint16_t id;
    size_t lens = dns_request_pack_tcp(buf, domain, ipv6, &id);
    if (0 == lens) {
        ev_close(&task->loader->netev, fd, skid, 1);
        return NULL;
    }
    size_t rsize = 0;
    void *resp = coro_send(task, fd, skid, buf, lens, &rsize, 1);
    ev_close(&task->loader->netev, fd, skid, 1);
    if (NULL == resp) {
        return NULL;
    }
    return dns_parse_pack(resp, rsize, cnt, id, NULL);
}
dns_ip *dns_lookup(task_ctx *task, const char *domain, int32_t ipv6, int32_t udp, size_t *cnt) {
    *cnt = 0;// 任何失败路径都不再往下写 cnt,统一在此归零,保证"返回 NULL 时 cnt 为 0"
    if (udp) {
        int32_t nodata = 0;
        dns_ip *ips = _dns_lookup_udp(task, domain, ipv6, cnt, &nodata);
        if (NULL != ips) {
            return ips;
        }
        // 服务端已明确答复"该域名没有这种记录",换 TCP 重查拿到的是同一个答复,白跑一趟
        if (0 != nodata) {
            return NULL;
        }
    }
    return _dns_lookup_tcp(task, domain, ipv6, cnt);
}
// ws:// 或 wss:// URL 解析与 scheme 校验；host 回填 NUL 结尾主机名(IPv6 字面量保留方括号)，*iswss 回填 scheme 是否 wss
static int32_t _ws_parse_url(url_ctx *url, const char *ws, struct evssl_ctx *evssl,
                             int32_t *iswss, char *host, size_t hostlens) {
    if (ERR_OK != url_parse(url, ws, strlen(ws), '/', 0)) {
        return ERR_FAILED;
    }
    int32_t isws = buf_icompare(&url->scheme, "ws", strlen("ws"));
    *iswss = buf_icompare(&url->scheme, "wss", strlen("wss"));
    if (!isws && !*iswss) {
        return ERR_FAILED;
    }
    if (*iswss && NULL == evssl) {
        return ERR_FAILED;
    }
    if (0 == url->host.lens
        || url->host.lens >= hostlens - 7) {// 预留 ":65535" + '\0'，供 _ws_reorg 就地追加端口
        return ERR_FAILED;
    }
    memcpy(host, url->host.data, url->host.lens);
    host[url->host.lens] = '\0';
    return ERR_OK;
}
// host 解析为连接用 ip(缓冲须为 IP_LENS 字节)，端口取 url 显式值或按 scheme 默认(RFC 6455 §3：ws 80 / wss 443)
static int32_t _ws_resolve_addr(task_ctx *task, url_ctx *url, const char *host, int32_t iswss,
                                char *ip, uint16_t *port) {
    ZERO(ip, IP_LENS);
    size_t hlens = strlen(host);
    // IPv6 字面量按 RFC 3986 §3.2.2 带方括号,连接地址须剥离(host 保留原始形式供 Host 头用)
    if ('[' == host[0]
        && hlens > 1
        && ']' == host[hlens - 1]) {
        if (hlens - 2 >= IP_LENS) {
            return ERR_FAILED;
        }
        memcpy(ip, host + 1, hlens - 2);
    } else if (ERR_OK != is_ipaddr(host)) {
        size_t nips;
        dns_ip *ips = dns_lookup(task, host, 0, 0, &nips);
        if (NULL == ips) {
            return ERR_FAILED;
        }
        if (0 == nips) {
            FREE(ips);
            return ERR_FAILED;
        }
        memcpy(ip, ips[0].ip, strlen(ips[0].ip));
        FREE(ips);
    } else {
        memcpy(ip, host, hlens);
    }
    if (url->port.lens > 0) {
        // url_parse 只按冒号切分不校验字符,strtoul 会把 "80abc" 当 80 接受;
        // RFC 3986 §3.2.3 的 port 产生式只允许数字,与 Lua 侧 ^%d+$ 对齐
        // port 是切片不带 \0，须按 lens 解析：strtoul 会一路读到缓冲里的下一个非数字
        uint64_t p;
        if (ERR_OK != str2u64((const char *)url->port.data, url->port.lens, UINT16_MAX, &p)
            || 0 == p) {
            return ERR_FAILED;
        }
        *port = (uint16_t)p;
    } else {
        *port = (0 != iswss) ? 443 : 80;
    }
    return ERR_OK;
}
// 就地在 host 末尾补非默认端口，并把 path + query 重组为 HTTP request-target 写入 uri
static void _ws_reorg(url_ctx *url, int32_t iswss, uint16_t port,
                      char *host, size_t hostlens, char *uri, size_t urilens) {
    // Host 头须带非默认端口(RFC 6455 §4.1)，否则严格服务端 / vhost 路由按纯主机名拒握手
    if (url->port.lens > 0
        && port != ((0 != iswss) ? 443 : 80)) {
        size_t hlens = strlen(host);
        SNPRINTF(host + hlens, hostlens - hlens, ":%d", (int32_t)port);
    }
    size_t plen = url_reorg_path(url, uri, urilens);
    if (0 == plen) {
        uri[plen++] = '/';
        uri[plen] = '\0';
    }
    if (plen + 1 < urilens) {
        uri[plen] = '?';
        size_t qlen = url_reorg_param(url, uri + plen + 1, urilens - plen - 1);
        if (0 == qlen) {
            uri[plen] = '\0';
        }
    }
}
// 打握手包并连接，发出后等服务端 Upgrade 响应；成功返回 fd 并回填 *skid / *spctx
static SOCKET _ws_handshake(task_ctx *task, struct evssl_ctx *evssl, const char *ip, uint16_t port,
                            int32_t netev, const char *host, const char *uri, const char *secprot,
                            uint64_t *skid, ws_secprots_ctx **spctx) {
    ws_hs_ctx *hsctx;
    char *reqpack = websock_pack_handshake(host, uri, secprot, &hsctx);
    if (NULL == reqpack) {
        return INVALID_SOCK;
    }
    SOCKET fd;
    if (ERR_OK != coro_connect(task, PACK_WEBSOCK, evssl, ip, port, netev, hsctx, &fd, skid)) {
        FREE(reqpack);
        return INVALID_SOCK;
    }
    if (ERR_OK != ev_send(&task->loader->netev, fd, *skid, reqpack, strlen(reqpack), 0)) {
        return INVALID_SOCK;
    }
    int32_t err;
    ws_secprots_ctx *sp = coro_handshaked(task, fd, *skid, &err, NULL);
    if (ERR_OK != err) {
        return INVALID_SOCK;
    }
    SET_PTR(spctx, sp);
    return fd;
}
SOCKET wbsock_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ws, const char *secprot,
    int32_t netev, uint64_t *skid, ws_secprots_ctx **spctx) {
    SET_PTR(spctx, NULL);
    url_ctx url;
    int32_t iswss;
    char host[HOST_LENS + 8];// 主机名 + ":65535" + '\0'
    if (ERR_OK != _ws_parse_url(&url, ws, evssl, &iswss, host, sizeof(host))) {
        return INVALID_SOCK;
    }
    char ip[IP_LENS];
    uint16_t port;
    if (ERR_OK != _ws_resolve_addr(task, &url, host, iswss, ip, &port)) {
        return INVALID_SOCK;
    }
    char uristack[URL_BUF_LENS];
    char *uribuf = uristack;
    size_t urilens = url.pathlens + url.paramlens + 3;
    if (urilens > sizeof(uristack)) {
        MALLOC(uribuf, urilens);
    } else {
        urilens = sizeof(uristack);
    }
    _ws_reorg(&url, iswss, port, host, sizeof(host), uribuf, urilens);
    SOCKET fd = _ws_handshake(task, evssl, ip, port, netev, host, uribuf, secprot, skid, spctx);
    if (uribuf != uristack) {
        FREE(uribuf);
    }
    return fd;
}
SOCKET redis_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ip, uint16_t port,
    const char *key, int32_t netev, uint64_t *skid) {
    SOCKET fd;
    if (ERR_OK != coro_connect(task, PACK_REDIS, evssl, ip, port, netev, NULL, &fd, skid)) {
        return INVALID_SOCK;
    }
    if (!EMPTYSTR(key)) {
        size_t size;
        char *auth = redis_pack(&size, "AUTH %s", key);
        redis_pack_ctx *rtn = coro_send(task, fd, *skid, auth, size, NULL, 0);
        if (NULL == rtn) {
            return INVALID_SOCK;
        }
        if (RESP_STRING != rtn->prot
            || 2 != rtn->len
            || 0 != _memicmp(rtn->data, "ok", (size_t)rtn->len)) {
            ev_close(&task->loader->netev, fd, *skid, 1);
            return INVALID_SOCK;
        }
    }
    return fd;
}
static int32_t _mysql_do_connect(task_ctx *task, mysql_ctx *mysql) {
    if (ERR_OK != mysql_try_connect(task, mysql, 1)) {
        return ERR_FAILED;
    }
    if (ERR_OK != coro_wait_connect(task, mysql->client.sk.fd, mysql->client.sk.skid, mysql->client.evssl)) {
        return ERR_FAILED;
    }
    int32_t err;
    char *errmsg = coro_handshaked(task, mysql->client.sk.fd, mysql->client.sk.skid, &err, NULL);
    if (ERR_OK != err) {
        if (NULL != errmsg) {
            LOG_WARN("%s", errmsg);
        }
    }
    return err;
}
// 命令串行化的进出，成对使用：调用方进函数时先 held = xxx->serial 捏住指针，加锁解锁都用它。
// held 为 NULL 表示这条连接不由 coro_utils 这套 API 管（经 Lua 绑定的 *_try_connect 建立时
// 就是这样），此时退化为原来的无锁行为。
// 之所以捏指针而不是解锁时重读 xxx->serial：*_quit 会先把字段置空，重读就会跳过 leave，
// 而 coro_serial_free 撞上持锁者时正指望那次 leave 来释放。
// lock 失败（不在协程内 / 连接正在销毁）时调用方一律按失败返回，不得再调 unlock。
// mysql / pgsql / mongo / smtp 四家同一套语义，故只此一份
static int32_t _serial_lock(coro_serial_ctx *held) {
    return NULL == held ? ERR_OK : coro_serial_enter(held);
}
static void _serial_unlock(coro_serial_ctx *held) {
    if (NULL != held) {
        coro_serial_leave(held);
    }
}
// serial 建在 try_connect 之前并立刻上锁:它一装上就对别的协程可见,而握手要多次挂起,
// 中间放人进来就是往半成品连接上发命令——而且等待按 (skid, mtype) 严格队头匹配,
// 它那条 RECV 会排在握手的 HANDSHAKED 之前,握手消息队头不匹配被丢弃,连接方挂到超时。
// try_connect 还会无条件覆写 sk.fd/skid,两个并发 connect 会互相孤立对方的 socket。
// 重连路径(ping 内)进来时已持锁,按 ref 计数嵌套;serial 只在首次创建,重建会把排队者连同锁一起丢掉
int32_t mysql_connect(task_ctx *task, mysql_ctx *mysql) {
    if (NULL == mysql->serial) {
        mysql->serial = coro_serial_new(task);
    }
    coro_serial_ctx *held = mysql->serial;
    if (ERR_OK != _serial_lock(held)) {
        return ERR_FAILED;
    }
    int32_t rtn = _mysql_do_connect(task, mysql);
    _serial_unlock(held);
    return rtn;
}
// 统一"发送+同步等待响应+校验 MPACK_OK"尾块;成功返回 ERR_OK,失败返回 ERR_FAILED
static int32_t _mysql_call(mysql_ctx *mysql, void *pack, size_t size) {
    mpack_ctx *mpack = coro_send(mysql->task, mysql->client.sk.fd, mysql->client.sk.skid, pack, size, NULL, 0);
    if (NULL == mpack) {
        return ERR_FAILED;
    }
    return MPACK_OK == mpack->pack_type ? ERR_OK : ERR_FAILED;
}
static int32_t _mysql_selectdb(mysql_ctx *mysql, const char *database) {
    size_t size;
    void *selectdb = mysql_pack_selectdb(mysql, database, &size);
    if (NULL == selectdb) {
        return ERR_FAILED;
    }
    return _mysql_call(mysql, selectdb, size);
}
int32_t mysql_selectdb(mysql_ctx *mysql, const char *database) {
    coro_serial_ctx *held = mysql->serial;
    if (ERR_OK != _serial_lock(held)) {
        return ERR_FAILED;
    }
    int32_t rtn = _mysql_selectdb(mysql, database);
    _serial_unlock(held);
    return rtn;
}
// 向 MySQL 服务器发送 ping 包并等待响应，失败返回 ERR_FAILED
static int32_t _mysql_ping(mysql_ctx *mysql) {
    size_t size;
    void *ping = mysql_pack_ping(mysql, &size);
    return _mysql_call(mysql, ping, size);
}
int32_t mysql_ping(mysql_ctx *mysql) {
    coro_serial_ctx *held = mysql->serial;
    if (ERR_OK != _serial_lock(held)) {
        return ERR_FAILED;
    }
    // 重连整段也在锁内：连接正在重建时别人不该往上发命令,而 fd/skid 换掉之后
    // 排队者醒来拿到的自然是新连接
    int32_t rtn = ERR_OK;
    if (ERR_OK != _mysql_ping(mysql)) {
        coro_close(mysql->task, mysql->client.sk.fd, mysql->client.sk.skid, 1);
        rtn = mysql_connect(mysql->task, mysql);
    }
    _serial_unlock(held);
    return rtn;
}
static mpack_ctx *_mysql_query(mysql_ctx *mysql, const char *sql, mysql_bind_ctx *mbind) {
    size_t size;
    void *query = mysql_pack_query(mysql, sql, mbind, &size);
    if (NULL == query) {
        return NULL;
    }
    return coro_send(mysql->task, mysql->client.sk.fd, mysql->client.sk.skid, query, size, NULL, 0);
}
// 逐个结果集回调,直到 more 为 0。四条约定:
// 1) more 在 cb 之前读——cb 里若调 mysql_reader_init 会把 mpack->pack 摘走,与 Lua 侧
//    _read_results 的"has_more 须在 reader.new 前读"同口径
// 2) cb 返回失败只记标志、循环照常把剩余包排空:半途不读完会把包留在连接缓冲里,
//    下一次查询直接 desync（Lua 侧踩过这个坑,见 mysql.lua 的同名注释）
// 3) 续读断连则无从排空,连接已废,直接失败返回
// 4) cb 为 NULL 表示只排空不回调,给 INSERT/UPDATE 这类不关心结果集的调用省一个空回调
static int32_t _mysql_read_results(mysql_ctx *mysql, mpack_ctx *mpack, mysql_result_cb cb, void *udata) {
    int32_t failed = 0;
    int32_t more;
    for (;;) {
        more = mysql_more(mpack);
        if (NULL != cb
            && ERR_OK != cb(mpack, udata)) {
            failed = 1;
        }
        if (0 == more) {
            break;
        }
        mpack = (mpack_ctx *)coro_recv(mysql->task, mysql->client.sk.fd, mysql->client.sk.skid, NULL);
        if (NULL == mpack) {
            return ERR_FAILED;
        }
    }
    return 0 != failed ? ERR_FAILED : ERR_OK;
}
int32_t mysql_query(mysql_ctx *mysql, const char *sql, mysql_bind_ctx *mbind,
                    mysql_result_cb cb, void *udata) {
    coro_serial_ctx *held = mysql->serial;
    if (ERR_OK != _serial_lock(held)) {
        return ERR_FAILED;
    }
    int32_t rtn = ERR_FAILED;
    mpack_ctx *mpack = _mysql_query(mysql, sql, mbind);
    if (NULL != mpack) {
        rtn = _mysql_read_results(mysql, mpack, cb, udata);
    }
    _serial_unlock(held);
    return rtn;
}
static mysql_stmt_ctx *_mysql_stmt_prepare(mysql_ctx *mysql, const char *sql) {
    size_t size;
    void *prepare = mysql_pack_stmt_prepare(mysql, sql, &size);
    if (NULL == prepare) {
        return NULL;
    }
    mpack_ctx *mpack = coro_send(mysql->task, mysql->client.sk.fd, mysql->client.sk.skid, prepare, size, NULL, 0);
    return mysql_stmt_init(mpack);
}
mysql_stmt_ctx *mysql_stmt_prepare(mysql_ctx *mysql, const char *sql) {
    coro_serial_ctx *held = mysql->serial;
    if (ERR_OK != _serial_lock(held)) {
        return NULL;
    }
    mysql_stmt_ctx *rtn = _mysql_stmt_prepare(mysql, sql);
    _serial_unlock(held);
    return rtn;
}
static mpack_ctx *_mysql_stmt_execute(mysql_stmt_ctx *stmt, mysql_bind_ctx *mbind) {
    size_t size;
    void *exec = mysql_pack_stmt_execute(stmt, mbind, &size);
    if (NULL == exec) {
        return NULL;
    }
    return coro_send(stmt->mysql->task, stmt->mysql->client.sk.fd, stmt->mysql->client.sk.skid, exec, size, NULL, 0);
}
int32_t mysql_stmt_execute(mysql_stmt_ctx *stmt, mysql_bind_ctx *mbind,
                           mysql_result_cb cb, void *udata) {
    mysql_ctx *mysql = stmt->mysql;
    coro_serial_ctx *held = mysql->serial;
    if (ERR_OK != _serial_lock(held)) {
        return ERR_FAILED;
    }
    int32_t rtn = ERR_FAILED;
    mpack_ctx *mpack = _mysql_stmt_execute(stmt, mbind);
    if (NULL != mpack) {
        rtn = _mysql_read_results(mysql, mpack, cb, udata);
    }
    _serial_unlock(held);
    return rtn;
}
int32_t mysql_stmt_reset(mysql_stmt_ctx *stmt) {
    coro_serial_ctx *held = stmt->mysql->serial;
    if (ERR_OK != _serial_lock(held)) {
        return ERR_FAILED;
    }
    size_t size;
    void *resetpk = mysql_pack_stmt_reset(stmt, &size);
    int32_t rtn = _mysql_call(stmt->mysql, resetpk, size);
    _serial_unlock(held);
    return rtn;
}
// 组包发送并释放 stmt。fd == INVALID_SOCK 表示连接已关闭;skid 变了表示中途重连过——
// stmt_id 是服务端按连接分配的,旧 id 发到新连接上会把恰好占用该 id 的语句关掉。
// 两种情况都跳过发包,但本地照样释放
static void _mysql_stmt_close(mysql_stmt_ctx *stmt) {
    mysql_ctx *mysql = stmt->mysql;
    size_t size;
    void *close = mysql_pack_stmt_close(stmt, &size);
    if (INVALID_SOCK == mysql->client.sk.fd
        || stmt->skid != mysql->client.sk.skid) {
        FREE(close);
    } else {
        ev_send(&mysql->task->loader->netev, mysql->client.sk.fd, mysql->client.sk.skid, close, size, 0);
    }
    mysql_stmt_free(stmt);
}
void mysql_stmt_close(mysql_stmt_ctx *stmt) {
    mysql_ctx *mysql = stmt->mysql;
    coro_serial_ctx *held = mysql->serial;
    // 两种"发不得"的情形都只做本地释放。COM_STMT_CLOSE 虽无响应,插在别人结果集流
    // 中间同样违反半双工;而组包那一步还会把连接的包序号清零,更不能在没有发言权时做。
    // 服务端那份语句会随连接关闭一并回收。
    // held 为 NULL 是关键的一种:那表示这条连接不由本套 API 串行化(Lua 绑定建的都是),
    // 而本函数正是 Lua 侧 stmt 的 __gc。__gc 在任意一次分配上同步触发、跑在工作线程,
    // 握手期间网络线程恰好也在递增 mysql->id —— 走发包路径就是一次跨线程写,
    // 表现为下一个握手包序号错位、报 "Got packets out of order"
    if (NULL == held
        || ERR_OK != coro_serial_enter(held)) {
        mysql_stmt_free(stmt);
        return;
    }
    _mysql_stmt_close(stmt);
    coro_serial_leave(held);
}
// 摘指针紧跟捕获、中间不能有挂起：拿到非 NULL 的只会是一个协程，它独占回收权。
// 本协程若正持着锁(如在结果集回调里调到这儿)，free 只标记不释放，真正回收落在紧随的 unlock
void mysql_quit(mysql_ctx *mysql) {
    coro_serial_ctx *held = mysql->serial;
    mysql->serial = NULL;
    // 摘到 NULL 说明另一个协程已经把执行器拿走、正在销毁同一条连接，断连与回收都归先到的
    // 那一方，本次直接退——再往下走会拿 _serial_lock(NULL) 的"无需串行化"放行，
    // 把命令无锁插进别人正在进行的交换里。
    // 拿不到锁（held 非 NULL 却 enter 失败，只可能是不在协程内）时不发命令只断连：
    // 组包那一步会把连接的包序号清零，没有发言权时不能做
    if (NULL == held) {
        return;
    }
    if (ERR_OK != _serial_lock(held)) {
        // 只可能是不在协程内(held 非 NULL 就排除了"别人正在销毁")——而往下的 coro_close
        // 内部要 mco_yield，同样得在协程里，硬走会撞断言。至少把执行器回收掉再退，
        // socket 留给对端关或 task 关闭时兜底
        coro_serial_free(held);
        return;
    }
    if (INVALID_SOCK != mysql->client.sk.fd) {
        size_t size;
        void *quit = mysql_pack_quit(mysql, &size);
        ev_send(&mysql->task->loader->netev, mysql->client.sk.fd, mysql->client.sk.skid, quit, size, 0);
        coro_close(mysql->task, mysql->client.sk.fd, mysql->client.sk.skid, 0);
    }
    coro_serial_free(held);
    _serial_unlock(held);
}
static int32_t _smtp_do_connect(task_ctx *task, smtp_ctx *smtp) {
    if (ERR_OK != smtp_try_connect(task, smtp, 1)) {
        return ERR_FAILED;
    }
    if (ERR_OK != coro_wait_connect(task, smtp->sk.fd, smtp->sk.skid, smtp->evssl)) {
        return ERR_FAILED;
    }
    int32_t err;
    char *msg = (char *)coro_handshaked(task, smtp->sk.fd, smtp->sk.skid, &err, NULL);
    if (ERR_OK != err) {
        if (NULL != msg) {
            LOG_WARN("%s", msg);
        }
    }
    return err;
}
// 建 serial 与整段握手都在锁内,理由同 mysql_connect
int32_t smtp_connect(task_ctx *task, smtp_ctx *smtp) {
    if (NULL == smtp->serial) {
        smtp->serial = coro_serial_new(task);
    }
    coro_serial_ctx *held = smtp->serial;
    if (ERR_OK != _serial_lock(held)) {
        return ERR_FAILED;
    }
    int32_t rtn = _smtp_do_connect(task, smtp);
    _serial_unlock(held);
    return rtn;
}
// 发送 SMTP QUIT 命令并等待响应（不关闭 socket）
static void _smtp_quit(smtp_ctx *smtp) {
    char *cmd = smtp_pack_quit();
    char *pack = coro_send(smtp->task, smtp->sk.fd, smtp->sk.skid, cmd, strlen(cmd), NULL, 0);
    if (NULL == pack) {
        return;
    }
    smtp_check_code(pack, "221");
}
void smtp_quit(smtp_ctx *smtp) {
    coro_serial_ctx *held = smtp->serial;
    smtp->serial = NULL;
    // 摘到 NULL 说明另一个协程已经把执行器拿走、正在销毁同一条连接。这里必须直接退:
    // 再往下走会拿 _serial_lock(NULL) 的"无需串行化"放行,把一条 QUIT 无锁插进
    // 那位或别人正在进行的邮件流里,随后的 coro_close 还会把对方半途的等待打断。
    // 断连与回收都归先到的那一方,本次什么都不用做
    if (NULL == held) {
        return;
    }
    if (ERR_OK != _serial_lock(held)) {
        // 只可能是不在协程内(held 非 NULL 就排除了"别人正在销毁")——而往下的 coro_close
        // 内部要 mco_yield，同样得在协程里，硬走会撞断言。至少把执行器回收掉再退，
        // socket 留给对端关或 task 关闭时兜底
        coro_serial_free(held);
        return;
    }
    if (INVALID_SOCK != smtp->sk.fd) {
        _smtp_quit(smtp);
        coro_close(smtp->task, smtp->sk.fd, smtp->sk.skid, 0);
    }
    // 持锁时 free 只标记不释放,真正回收落在紧随的 unlock
    coro_serial_free(held);
    _serial_unlock(held);
}
// 发送 SMTP NOOP 命令检测连接是否存活，失败返回 ERR_FAILED
static int32_t _smtp_ping(smtp_ctx *smtp) {
    char *cmd = smtp_pack_ping();
    char *pack = coro_send(smtp->task, smtp->sk.fd, smtp->sk.skid, cmd, strlen(cmd), NULL, 0);
    if (NULL == pack) {
        return ERR_FAILED;
    }
    return smtp_check_ok(pack);
}
int32_t smtp_ping(smtp_ctx *smtp) {
    coro_serial_ctx *held = smtp->serial;
    if (ERR_OK != _serial_lock(held)) {
        return ERR_FAILED;
    }
    // 重连整段也在锁内：连接正在重建时别人不该往上发命令,而 fd/skid 换掉之后
    // 排队者醒来拿到的自然是新连接
    int32_t rtn = ERR_OK;
    if (ERR_OK != _smtp_ping(smtp)) {
        coro_close(smtp->task, smtp->sk.fd, smtp->sk.skid, 1);
        rtn = smtp_connect(smtp->task, smtp);
    }
    _serial_unlock(held);
    return rtn;
}
// 执行 SMTP 邮件发送流程（MAIL FROM → RCPT TO → DATA → 正文）
static int32_t _smtp_send(smtp_ctx *smtp, mail_ctx *mail) {
    //发件人地址含 CRLF 时 smtp_pack_from 返回 NULL，拒绝发送
    char *cmd = smtp_pack_from(mail->from.addr);
    if (NULL == cmd) {
        return ERR_FAILED;
    }
    char *pack = coro_send(smtp->task, smtp->sk.fd, smtp->sk.skid, cmd, strlen(cmd), NULL, 0);
    if (NULL == pack
        || ERR_OK != smtp_check_ok(pack)) {
        return ERR_FAILED;
    }
    uint32_t naddr = array_size(&mail->addrs);
    for (uint32_t i = 0; i < naddr; i++) {
        //收件人地址含 CRLF 时 smtp_pack_rcpt 返回 NULL，拒绝发送
        cmd = smtp_pack_rcpt(((mail_addr *)array_at(&mail->addrs, i))->addr);
        if (NULL == cmd) {
            return ERR_FAILED;
        }
        pack = coro_send(smtp->task, smtp->sk.fd, smtp->sk.skid, cmd, strlen(cmd), NULL, 0);
        if (NULL == pack
            || ERR_OK != smtp_check_ok(pack)) {
            return ERR_FAILED;
        }
    }
    cmd = smtp_pack_data();
    pack = coro_send(smtp->task, smtp->sk.fd, smtp->sk.skid, cmd, strlen(cmd), NULL, 0);
    if (NULL == pack) {
        return ERR_FAILED;
    }
    if (ERR_OK != smtp_check_code(pack, "354")) {
        return ERR_FAILED;
    }
    cmd = mail_pack(mail);
    pack = coro_send(smtp->task, smtp->sk.fd, smtp->sk.skid, cmd, strlen(cmd), NULL, 0);
    if (NULL == pack
        || ERR_OK != smtp_check_ok(pack)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 发送 SMTP RSET 命令重置会话状态（不关闭连接）
static int32_t _smtp_reset(smtp_ctx *smtp) {
    char *cmd = smtp_pack_reset();
    char *pack = coro_send(smtp->task, smtp->sk.fd, smtp->sk.skid, cmd, strlen(cmd), NULL, 0);
    if (NULL == pack) {
        return ERR_FAILED;
    }
    return smtp_check_ok(pack);
}
int32_t smtp_send(smtp_ctx *smtp, mail_ctx *mail) {
    coro_serial_ctx *held = smtp->serial;
    if (ERR_OK != _serial_lock(held)) {
        return ERR_FAILED;
    }
    // 锁覆盖 _smtp_send + _smtp_reset 整段:RSET 清的是本次投递在服务端留下的会话状态,
    // 与发送是同一笔事。分开各包一次的话,别人的 MAIL FROM 会挤在中间被我们的 RSET 清掉
    int32_t rtn = _smtp_send(smtp, mail);
    _smtp_reset(smtp);
    _serial_unlock(held);
    return rtn;
}
static int32_t _pgsql_do_connect(task_ctx *task, pgsql_ctx *pg) {
    if (ERR_OK != pgsql_try_connect(task, pg, 1)) {
        return ERR_FAILED;
    }
    // pgsql SSL 是协议层收到服务端 'S' 应答后才发起(见 _pgsql_ssl_response)，此处不能传 pg->evssl，
    // 否则会等一个尚未触发的 SSLEXCHANGED 直到超时；coro_handshaked 的等待自然跨过该升级过程
    if (ERR_OK != coro_wait_connect(task, pg->sk.fd, pg->sk.skid, NULL)) {
        return ERR_FAILED;
    }
    int32_t code;
    char *err = coro_handshaked(task, pg->sk.fd, pg->sk.skid, &code, NULL);
    if (ERR_OK != code) {
        if (NULL != err) {
            LOG_WARN("%s", err);
        }
    }
    return code;
}
// 建 serial 与整段握手都在锁内,理由同 mysql_connect
int32_t pgsql_connect(task_ctx *task, pgsql_ctx *pg) {
    if (NULL == pg->serial) {
        pg->serial = coro_serial_new(task);
    }
    coro_serial_ctx *held = pg->serial;
    if (ERR_OK != _serial_lock(held)) {
        return ERR_FAILED;
    }
    int32_t rtn = _pgsql_do_connect(task, pg);
    _serial_unlock(held);
    return rtn;
}
int32_t pgsql_cancel(pgsql_ctx *pg) {
    if (INVALID_SOCK == pg->sk.fd || 0 == pg->pid) {
        return ERR_FAILED;
    }
    SOCKET fd;
    uint64_t skid;
    // CancelRequest 须在独立 TCP 连接上发送，服务端处理后主动关闭连接，无任何响应
    if (ERR_OK != coro_connect(pg->task, PACK_NONE, NULL, pg->ip, pg->port, 0, NULL, &fd, &skid)) {
        return ERR_FAILED;
    }
    char buf[16];
    pgsql_pack_cancel(buf, pg->pid, pg->key);
    int32_t rtn = ev_send(&pg->task->loader->netev, fd, skid, buf, sizeof(buf), 1);
    ev_close(&pg->task->loader->netev, fd, skid, 0);
    return rtn;
}
// 断开连接但不动 serial：selectdb 靠断连重连来换库,那期间锁还在本协程手上,
// 顺手把执行器销毁掉就把自己的锁毁了
static void _pgsql_disconnect(pgsql_ctx *pg) {
    if (INVALID_SOCK == pg->sk.fd) {
        return;
    }
    size_t lens;
    void *quit = pgsql_pack_terminate(&lens);
    ev_send(&pg->task->loader->netev, pg->sk.fd, pg->sk.skid, quit, lens, 0);
    coro_close(pg->task, pg->sk.fd, pg->sk.skid, 0);
}
// 摘指针、包锁、推迟释放的道理同 mysql_quit，不重复
void pgsql_quit(pgsql_ctx *pg) {
    coro_serial_ctx *held = pg->serial;
    pg->serial = NULL;
    if (NULL == held) {
        return;
    }
    if (ERR_OK != _serial_lock(held)) {
        // 只可能是不在协程内(held 非 NULL 就排除了"别人正在销毁")——而往下的 coro_close
        // 内部要 mco_yield，同样得在协程里，硬走会撞断言。至少把执行器回收掉再退，
        // socket 留给对端关或 task 关闭时兜底
        coro_serial_free(held);
        return;
    }
    _pgsql_disconnect(pg);
    coro_serial_free(held);
    _serial_unlock(held);
}
int32_t pgsql_selectdb(pgsql_ctx *pg, const char *database) {
    // 换库整段在锁内(含 set_db)：它改的 pg->database 是连接级状态,搁在锁外的话拿不到锁那次
    // 会留下"库名已换、连接还在旧库上",之后随便哪次 ping 重连就悄悄换了库
    coro_serial_ctx *held = pg->serial;
    if (ERR_OK != _serial_lock(held)) {
        return ERR_FAILED;
    }
    // 先校验再断连：库名超长时 set_db 保留旧名,若照旧先断就白断一条可用连接,
    // 还会用原库名重连成功、把切库失败报成功
    int32_t rtn = pgsql_set_db(pg, database);
    if (ERR_OK == rtn) {
        // fd/skid 换掉之后排队者醒来拿到的自然是新连接
        _pgsql_disconnect(pg);
        rtn = pgsql_connect(pg->task, pg);
    }
    _serial_unlock(held);
    return rtn;
}
// 一条 simple query：发出去等到 ReadyForQuery 才算一次完整往返
static pgpack_ctx *_pgsql_query(pgsql_ctx *pg, const char *sql) {
    size_t lens;
    void *query = pgsql_pack_query(sql, &lens);
    return coro_send(pg->task, pg->sk.fd, pg->sk.skid, query, lens, NULL, 0);
}
int32_t pgsql_ping(pgsql_ctx *pg) {
    coro_serial_ctx *held = pg->serial;
    if (ERR_OK != _serial_lock(held)) {
        return ERR_FAILED;
    }
    int32_t rtn = ERR_OK;
    if (NULL == _pgsql_query(pg, ";")) {
        coro_close(pg->task, pg->sk.fd, pg->sk.skid, 1);
        rtn = pgsql_connect(pg->task, pg);
    }
    _serial_unlock(held);
    return rtn;
}
pgpack_ctx *pgsql_query(pgsql_ctx *pg, const char *sql) {
    coro_serial_ctx *held = pg->serial;
    if (ERR_OK != _serial_lock(held)) {
        return NULL;
    }
    pgpack_ctx *rtn = _pgsql_query(pg, sql);
    _serial_unlock(held);
    return rtn;
}
static int32_t _pgsql_stmt_prepare(pgsql_ctx *pg, const char *name, const char *sql, int16_t nparam, uint32_t *oids) {
    size_t lens;
    void *parse = pgsql_pack_stmt_prepare(name, sql, nparam, oids, &lens);
    pgpack_ctx *pgpack = coro_send(pg->task, pg->sk.fd, pg->sk.skid, parse, lens, NULL, 0);
    if (NULL == pgpack) {
        return ERR_FAILED;
    }
    if (PGPACK_ERR == pgpack->type) {
        LOG_WARN("%s", (const char *)pgpack->pack);
        return ERR_FAILED;
    }
    return PGPACK_OK == pgpack->type ? ERR_OK : ERR_FAILED;
}
int32_t pgsql_stmt_prepare(pgsql_ctx *pg, const char *name, const char *sql, int16_t nparam, uint32_t *oids) {
    if (EMPTYSTR(sql)) {
        return ERR_FAILED;
    }
    coro_serial_ctx *held = pg->serial;
    if (ERR_OK != _serial_lock(held)) {
        return ERR_FAILED;
    }
    int32_t rtn = _pgsql_stmt_prepare(pg, name, sql, nparam, oids);
    _serial_unlock(held);
    return rtn;
}
pgpack_ctx *pgsql_stmt_execute(pgsql_ctx *pg, const char *name, pgsql_bind_ctx *bind, pgpack_format resultformat) {
    coro_serial_ctx *held = pg->serial;
    if (ERR_OK != _serial_lock(held)) {
        return NULL;
    }
    size_t lens;
    void *exec = pgsql_pack_stmt_execute(name, bind, resultformat, &lens);
    pgpack_ctx *rtn = coro_send(pg->task, pg->sk.fd, pg->sk.skid, exec, lens, NULL, 0);
    _serial_unlock(held);
    return rtn;
}
void pgsql_stmt_close(pgsql_ctx *pg, const char *name) {
    coro_serial_ctx *held = pg->serial;
    if (ERR_OK != _serial_lock(held)) {
        return;// Close 也要等 CloseComplete,拿不到锁就别发,服务端那份随连接关闭一并回收
    }
    size_t lens;
    void *close = pgsql_pack_stmt_close(name, &lens);
    coro_send(pg->task, pg->sk.fd, pg->sk.skid, close, lens, NULL, 0);
    _serial_unlock(held);
}
static pgpack_ctx *_pgsql_copy_in(pgsql_ctx *pg, const char *sql, const void *data, size_t lens) {
    // 第一步：发送 COPY SQL，等待服务端返回 CopyInResponse（PGPACK_COPY_IN）
    size_t qsize;
    void *query = pgsql_pack_query(sql, &qsize);
    pgpack_ctx *pgpack = coro_send(pg->task, pg->sk.fd, pg->sk.skid, query, qsize, NULL, 0);
    if (NULL == pgpack || PGPACK_COPY_IN != pgpack->type) {
        return pgpack; // 服务端未进入 COPY IN 模式（通常为 PGPACK_ERR），直接返回调用方
    }
    // 第一次 coro_send 的返回值 pgpack 由框架在下次 yield 时经 _message_clean 自动释放，此处无需手动释放
    // 第二步：将 CopyData + CopyDone 合并为一个缓冲区，一次发送并等待 ReadyForQuery
    size_t dsize, csize;
    void *copy_data = pgsql_pack_copy_data(data, lens, &dsize);
    void *copy_done = pgsql_pack_copy_done(&csize);
    // 合并两段到连续缓冲区后发送，避免两次系统调用
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_binary(&bwriter, copy_data, dsize);
    binary_set_binary(&bwriter, copy_done, csize);
    FREE(copy_data);
    FREE(copy_done);
    return coro_send(pg->task, pg->sk.fd, pg->sk.skid, bwriter.data, bwriter.offset, NULL, 0);
}
pgpack_ctx *pgsql_copy_in(pgsql_ctx *pg, const char *sql, const void *data, size_t lens) {
    // 本函数是两次往返:服务端进 COPY IN 模式后,在 CopyDone 之前它只认 CopyData/CopyFail,
    // 中间插进别人一条普通查询就整条连接报错。锁必须覆盖两次往返,不能各自包一次
    coro_serial_ctx *held = pg->serial;
    if (ERR_OK != _serial_lock(held)) {
        return NULL;
    }
    pgpack_ctx *rtn = _pgsql_copy_in(pg, sql, data, lens);
    _serial_unlock(held);
    return rtn;
}
pgpack_ctx *pgsql_copy_out(pgsql_ctx *pg, const char *sql) {
    coro_serial_ctx *held = pg->serial;
    if (ERR_OK != _serial_lock(held)) {
        return NULL;
    }
    // 发送 COPY SQL，解析器在收到所有 CopyData + CopyDone 后于 ReadyForQuery 时返回累积结果
    size_t qsize;
    void *query = pgsql_pack_query(sql, &qsize);
    pgpack_ctx *rtn = coro_send(pg->task, pg->sk.fd, pg->sk.skid, query, qsize, NULL, 0);
    _serial_unlock(held);
    return rtn;
}
// 事务会话的绑定不能跨连接存活：新连接一建立就解绑，之后组包侧的 TRANSACTION_OPTIONS 才不会
// 把上一代的 lsid/txnNumber 附到 hello 及后续命令上。
// 清在这里而不是断开时的 _mongo_udfree：那个回调跑在网络线程，而组包侧是在属主线程上
// 判 mongo->session 非空后解引用它的 options/started，跨线程置空会让那两步之间读到 NULL。
// 放在连接入口还顺带覆盖"在一条仍打开的连接上重入 connect"——那种情况根本不会触发 udfree
static int32_t _mongo_do_connect(task_ctx *task, mongo_ctx *mongo) {
    if (ERR_OK != mongo_try_connect(task, mongo, 1)) {
        return ERR_FAILED;
    }
    mongo_clear_session(mongo);
    return coro_wait_connect(task, mongo->sk.fd, mongo->sk.skid, mongo->evssl);
}
// 建 serial 与整段握手都在锁内,理由同 mysql_connect
int32_t mongo_connect(task_ctx *task, mongo_ctx *mongo) {
    if (NULL == mongo->serial) {
        mongo->serial = coro_serial_new(task);
    }
    coro_serial_ctx *held = mongo->serial;
    if (ERR_OK != _serial_lock(held)) {
        return ERR_FAILED;
    }
    int32_t rtn = _mongo_do_connect(task, mongo);
    _serial_unlock(held);
    return rtn;
}
// 摘指针、包锁、推迟释放的道理同 mysql_quit，不重复。
// mongo 没有 QUIT 命令，只有断连，所以拿不拿得到锁都照断——加锁是为了别把
// 别人半途的等待拦腰打断
void mongo_quit(mongo_ctx *mongo) {
    coro_serial_ctx *held = mongo->serial;
    mongo->serial = NULL;
    if (NULL == held) {
        return;
    }
    if (ERR_OK != _serial_lock(held)) {
        // 只可能是不在协程内(held 非 NULL 就排除了"别人正在销毁")——而往下的 coro_close
        // 内部要 mco_yield，同样得在协程里，硬走会撞断言。至少把执行器回收掉再退，
        // socket 留给对端关或 task 关闭时兜底
        coro_serial_free(held);
        return;
    }
    coro_close(mongo->task, mongo->sk.fd, mongo->sk.skid, 0);
    coro_serial_free(held);
    _serial_unlock(held);
}
// 执行 MongoDB SCRAM 认证流程（发送 client-first 消息并等待握手结果）
static int32_t _mongo_auth(mongo_ctx *mongo, const char *authmod) {
    if (ERR_OK != ev_ud_status(&mongo->task->loader->netev, mongo->sk.fd, mongo->sk.skid, mongo_status_auth())) {
        return ERR_FAILED;
    }
    size_t lens;
    void *client_first = mongo_pack_scram_client_first(mongo, authmod, &lens);
    if (NULL == client_first) {
        // scram 未初始化成功,回滚状态为 COMMAND,避免连接卡在 AUTH 态导致后续正常响应
        // 误入 _mongo_scram_auth 解引用 NULL scram(mongo.c 内已加判空兜底,此处是根因修复)
        ev_ud_status(&mongo->task->loader->netev, mongo->sk.fd, mongo->sk.skid, mongo_status_command());
        return ERR_FAILED;
    }
    if (ERR_OK != ev_send(&mongo->task->loader->netev, mongo->sk.fd, mongo->sk.skid, client_first, lens, 0)) {
        return ERR_FAILED;
    }
    int32_t err;
    coro_handshaked(mongo->task, mongo->sk.fd, mongo->sk.skid, &err, NULL);
    return err;
}
int32_t mongo_auth(mongo_ctx *mongo, const char *authmod, const char *user, const char *pwd) {
    // _mongo_auth 绕开两个漏斗(直接 ev_send + coro_handshaked 走协议层的 SCRAM 状态机),
    // 锁只能加在这里；期间连接处于 AUTH 态,别人的普通命令挤进来会被当成认证响应解析
    coro_serial_ctx *held = mongo->serial;
    if (ERR_OK != _serial_lock(held)) {
        return ERR_FAILED;
    }
    int32_t flags = mongo_clear_flag(mongo);
    int32_t rtn = mongo_user_pwd(mongo, user, pwd);
    if (ERR_OK == rtn) {
        safe_fill_str(mongo->authmod, sizeof(mongo->authmod), authmod);
        rtn = _mongo_auth(mongo, authmod);
    }
    mongo_set_flag(mongo, flags);
    _serial_unlock(held);
    return rtn;
}
// 统一"组包判空 + 发送 + 同步等待响应"(不受 MORETOCOME 影响,总是等待),不校验命令级错误:
// 调用方各有各的用法——count 要 n 值、startsession 要 session、commit/rollback 要凭"服务端
// 是否有响应"决定清不清事务状态。pack 为 NULL(组包被拒)在此一并吸收,与网络失败同样返回 NULL,
// 两者的处置在全部调用方那里恰好相同。对应 Lua 侧 mongo.lua 的 _rsend。
// 串行化也落在这里(与 _mongo_send 两处覆盖全部命令站点):各命令函数在 sendwait 之后只做纯解析,
// 不再有 I/O,所以锁到本函数为止与锁整个命令函数等效。mongo_auth / mongo_ping 另有外层锁,
// 它们走多次往返或绕开本漏斗,靠 ref 计数嵌套
static mgopack_ctx *_mongo_sendwait(mongo_ctx *mongo, void *pack, size_t lens) {
    if (NULL == pack) {
        return NULL;
    }
    coro_serial_ctx *held = mongo->serial;
    if (ERR_OK != _serial_lock(held)) {
        FREE(pack);
        return NULL;
    }
    mgopack_ctx *rtn = coro_send(mongo->task, mongo->sk.fd, mongo->sk.skid, pack, lens, NULL, 0);
    _serial_unlock(held);
    return rtn;
}
// 在 _mongo_sendwait 之上加命令级错误校验(服务端原因由 mongo_parse_check_error 打印);
// 成功返回 mgopack,失败返回 NULL
static mgopack_ctx *_mongo_call(mongo_ctx *mongo, void *pack, size_t lens) {
    mgopack_ctx *mgpack = _mongo_sendwait(mongo, pack, lens);
    if (NULL == mgpack) {
        return NULL;
    }
    if (ERR_FAILED == mongo_parse_check_error(mgpack)) {
        return NULL;
    }
    return mgpack;
}
mgopack_ctx *mongo_hello(mongo_ctx *mongo, char *options, size_t optlens) {
    int32_t flags = mongo_clear_flag(mongo);
    size_t lens;
    void *hello = mongo_pack_hello(mongo, options, optlens, &lens);
    mongo_set_flag(mongo, flags);
    return _mongo_call(mongo, hello, lens);
}
static int32_t _mongo_ping(mongo_ctx *mongo) {
    int32_t flags = mongo_clear_flag(mongo);
    size_t lens;
    void *ping = mongo_pack_ping(mongo, &lens);
    mongo_set_flag(mongo, flags);
    return NULL == _mongo_call(mongo, ping, lens) ? ERR_FAILED : ERR_OK;
}
int32_t mongo_ping(mongo_ctx *mongo) {
    // 重连 + hello + re-auth 整段在锁内:这几步是一笔不可分的重建,别人的命令插在中间
    // 会打在半成品连接上。内层的 _mongo_ping / mongo_hello 走漏斗,按 ref 计数嵌套
    coro_serial_ctx *held = mongo->serial;
    if (ERR_OK != _serial_lock(held)) {
        return ERR_FAILED;
    }
    int32_t rtn = ERR_OK;
    if (ERR_OK != _mongo_ping(mongo)) {
        coro_close(mongo->task, mongo->sk.fd, mongo->sk.skid, 1);
        if (ERR_OK != mongo_connect(mongo->task, mongo)) {
            rtn = ERR_FAILED;
        } else if (NULL == mongo_hello(mongo, NULL, 0)) {
            rtn = ERR_FAILED;
        } else if (0 != mongo->user[0]) {
            int32_t flags = mongo_clear_flag(mongo);
            rtn = _mongo_auth(mongo, mongo->authmod);// 不使用mongo_auth，它里面有psw user赋值
            mongo_set_flag(mongo, flags);
        }
    }
    _serial_unlock(held);
    return rtn;
}
// MongoDB 统一发送函数：设置了 MORETOCOME 标志时仅发送不等待响应，否则同步等待响应。
// pack 为 NULL(组包被拒)在此一并吸收,语义同 _mongo_call
// MORETOCOME 变体:置位时只发不等。同样在锁内发——不等响应也不能乱序,
// 后面那条 find 得看得见前面这批 insert
static int32_t _mongo_send(mongo_ctx *mongo, void *pack, size_t lens, mgopack_ctx **mgopack) {
    if (NULL == pack) {
        return ERR_FAILED;
    }
    coro_serial_ctx *held = mongo->serial;
    if (ERR_OK != _serial_lock(held)) {
        FREE(pack);
        return ERR_FAILED;
    }
    int32_t rtn = ERR_FAILED;
    if (mongo_check_flag(mongo, MORETOCOME)) {
        rtn = ev_send(&mongo->task->loader->netev, mongo->sk.fd, mongo->sk.skid, pack, lens, 0);
    } else {
        mgopack_ctx *rtnpack = coro_send(mongo->task, mongo->sk.fd, mongo->sk.skid, pack, lens, NULL, 0);
        if (NULL != rtnpack) {
            SET_PTR(mgopack, rtnpack);
            rtn = ERR_OK;
        }
    }
    _serial_unlock(held);
    return rtn;
}
int32_t mongo_drop(mongo_ctx *mongo, char *options, size_t optlens) {
    size_t lens;
    void *drop = mongo_pack_drop(mongo, options, optlens, &lens);
    mgopack_ctx *mgpack = NULL;
    if (ERR_OK != _mongo_send(mongo, drop, lens, &mgpack)) {
        return ERR_FAILED;
    }
    if (NULL == mgpack) {
        return ERR_OK;
    }
    if (ERR_FAILED == mongo_parse_check_error(mgpack)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t mongo_insert(mongo_ctx *mongo, char *docs, size_t dlens, char *options, size_t optlens) {
    size_t lens;
    void *insert = mongo_pack_insert(mongo, docs, dlens, options, optlens, &lens);
    mgopack_ctx *mgpack = NULL;
    if (ERR_OK != _mongo_send(mongo, insert, lens, &mgpack)) {
        return ERR_FAILED;
    }
    if (NULL == mgpack) {
        return ERR_OK;
    }
    return mongo_parse_check_error(mgpack);
}
int32_t mongo_update(mongo_ctx *mongo, char *updates, size_t ulens, char *options, size_t optlens) {
    size_t lens;
    void *update = mongo_pack_update(mongo, updates, ulens, options, optlens, &lens);
    mgopack_ctx *mgpack = NULL;
    if (ERR_OK != _mongo_send(mongo, update, lens, &mgpack)) {
        return ERR_FAILED;
    }
    if (NULL == mgpack) {
        return ERR_OK;
    }
    return mongo_parse_check_error(mgpack);
}
int32_t mongo_delete(mongo_ctx *mongo, char *deletes, size_t dlens, char *options, size_t optlens) {
    size_t lens;
    void *del = mongo_pack_delete(mongo, deletes, dlens, options, optlens, &lens);
    mgopack_ctx *mgpack = NULL;
    if (ERR_OK != _mongo_send(mongo, del, lens, &mgpack)) {
        return ERR_FAILED;
    }
    if (NULL == mgpack) {
        return ERR_OK;
    }
    return mongo_parse_check_error(mgpack);
}
mgopack_ctx *mongo_bulkwrite(mongo_ctx *mongo, char *ops, size_t olens, char *nsinfo, size_t nlens, char *options, size_t optlens) {
    size_t lens;
    void *bulkwrite = mongo_pack_bulkwrite(mongo, ops, olens, nsinfo, nlens, options, optlens, &lens);
    mgopack_ctx *mgpack = NULL;
    if (ERR_OK != _mongo_send(mongo, bulkwrite, lens, &mgpack)) {
        return NULL;
    }
    if (NULL == mgpack) {
        return NULL;
    }
    if (ERR_FAILED == mongo_parse_check_error(mgpack)) {
        return NULL;
    }
    return mgpack;
}
mgopack_ctx *mongo_find(mongo_ctx *mongo, char *filter, size_t flens, char *options, size_t optlens) {
    int32_t flags = mongo_clear_flag(mongo);
    size_t lens;
    void *find = mongo_pack_find(mongo, filter, flens, options, optlens, &lens);
    mongo_set_flag(mongo, flags);
    return _mongo_call(mongo, find, lens);
}
mgopack_ctx *mongo_aggregate(mongo_ctx *mongo, char *pipeline, size_t pllens, char *options, size_t optlens) {
    int32_t flags = mongo_clear_flag(mongo);
    size_t lens;
    void *aggt = mongo_pack_aggregate(mongo, pipeline, pllens, options, optlens, &lens);
    mongo_set_flag(mongo, flags);
    return _mongo_call(mongo, aggt, lens);
}
mgopack_ctx *mongo_getmore(mongo_ctx *mongo, int64_t cursorid, char *options, size_t optlens) {
    int32_t flags = mongo_clear_flag(mongo);
    size_t lens;
    void *getmore = mongo_pack_getmore(mongo, cursorid, options, optlens, &lens);
    mongo_set_flag(mongo, flags);
    return _mongo_call(mongo, getmore, lens);
}
mgopack_ctx *mongo_killcursors(mongo_ctx *mongo, char *cursorids, size_t cslens, char *options, size_t optlens) {
    size_t lens;
    void *killcursors = mongo_pack_killcursors(mongo, cursorids, cslens, options, optlens, &lens);
    mgopack_ctx *mgpack = NULL;
    if (ERR_OK != _mongo_send(mongo, killcursors, lens, &mgpack)) {
        return NULL;
    }
    if (NULL == mgpack) {
        return NULL;
    }
    if (ERR_FAILED == mongo_parse_check_error(mgpack)) {
        return NULL;
    }
    return mgpack;
}
mgopack_ctx *mongo_distinct(mongo_ctx *mongo, const char *key, char *query, size_t qlens, char *options, size_t optlens) {
    int32_t flags = mongo_clear_flag(mongo);
    size_t lens;
    void *distinct = mongo_pack_distinct(mongo, key, query, qlens, options, optlens, &lens);
    mongo_set_flag(mongo, flags);
    return _mongo_call(mongo, distinct, lens);
}
mgopack_ctx *mongo_findandmodify(mongo_ctx *mongo, char *query, size_t qlens,
    int32_t remove, int32_t pipeline, char *update, size_t ulens, char *options, size_t optlens) {
    int32_t flags = mongo_clear_flag(mongo);
    size_t lens;
    void *findandmodify = mongo_pack_findandmodify(mongo, query, qlens, remove, pipeline, update, ulens, options, optlens, &lens);
    mongo_set_flag(mongo, flags);
    return _mongo_call(mongo, findandmodify, lens);
}
int32_t mongo_count(mongo_ctx *mongo, char *query, size_t qlens, char *options, size_t optlens) {
    int32_t flags = mongo_clear_flag(mongo);
    size_t lens;
    void *count = mongo_pack_count(mongo, query, qlens, options, optlens, &lens);
    mongo_set_flag(mongo, flags);
    mgopack_ctx *mgpack = _mongo_sendwait(mongo, count, lens);
    if (NULL == mgpack) {
        return ERR_FAILED;
    }
    return mongo_parse_check_error(mgpack);
}
int32_t mongo_createindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t optlens) {
    size_t lens;
    void *createindexes = mongo_pack_createindexes(mongo, indexes, ilens, options, optlens, &lens);
    mgopack_ctx *mgpack = NULL;
    if (ERR_OK != _mongo_send(mongo, createindexes, lens, &mgpack)) {
        return ERR_FAILED;
    }
    if (NULL == mgpack) {
        return ERR_OK;
    }
    if (ERR_FAILED == mongo_parse_check_error(mgpack)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t mongo_dropindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t optlens) {
    size_t lens;
    void *dropindexes = mongo_pack_dropindexes(mongo, indexes, ilens, options, optlens, &lens);
    mgopack_ctx *mgpack = NULL;
    if (ERR_OK != _mongo_send(mongo, dropindexes, lens, &mgpack)) {
        return ERR_FAILED;
    }
    if (NULL == mgpack) {
        return ERR_OK;
    }
    if (ERR_FAILED == mongo_parse_check_error(mgpack)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
mongo_session *mongo_startsession(mongo_ctx *mongo) {
    int32_t flags = mongo_clear_flag(mongo);
    size_t lens;
    void *startsession = mongo_pack_startsession(mongo, &lens);
    mongo_set_flag(mongo, flags);
    mgopack_ctx *mgpack = _mongo_sendwait(mongo, startsession, lens);
    if (NULL == mgpack) {
        return NULL;
    }
    mongo_session *session;
    CALLOC(session, 1, sizeof(mongo_session));
    if (!mongo_parse_startsession(mgpack, session->uuid, &session->timeoutmin)) {
        FREE(session);
        return NULL;
    }
    session->mongo = mongo;
    session->txnnumber = 0;
    session->timeout = nowsec() + session->timeoutmin * 60;
    return session;
}
int32_t mongo_refreshsession(mongo_session *session) {
    mongo_ctx *mongo = session->mongo;
    int32_t flags = mongo_clear_flag(mongo);
    size_t lens;
    void *refreshsession = mongo_pack_refreshsession(session, &lens);
    mongo_set_flag(mongo, flags);
    if (NULL == _mongo_call(mongo, refreshsession, lens)) {
        return ERR_FAILED;
    }
    session->timeout = nowsec() + session->timeoutmin * 60;
    return ERR_OK;
}
void mongo_freesession(mongo_session *session) {
    mongo_ctx *mongo = session->mongo;
    size_t lens;
    void *endsession = mongo_pack_endsession(session, &lens);
    _mongo_send(mongo, endsession, lens, NULL);
    if (mongo->session == session) {
        mongo->session = NULL;
    }
    FREE(session->options);
    FREE(session);
}
// 事务绑定规则,begin / commit / rollback 三处共用:
// 组包侧 TRANSACTION_OPTIONS 与 TRANSACTION_OPTIONS_START 一律从 mongo->session 取事务上下文,
// 所以"连接当前绑定的 session"必须与调用方手上那个是同一个,否则命令会挂到别人的事务上。
// begin 靠拒绝第二个 session 维持它;commit/rollback 在入口挡掉已经分叉的情形——分叉来自
// 重连入口 mongo_connect 的解绑,或期间另一个 session 接管了这条连接。
// 挡掉而不是改用入参的 lsid 发出去:MongoDB 要求 commit/abort 发在事务所在的那条连接上,
// 连接已经换过,发什么都只会换回 NoSuchTransaction,不如省掉这个往返直接报失败。
// 早退不释放 session->options 也不漏:重新 begin 会先 FREE 一次,mongo_freesession 也会释放
int32_t mongo_begin(mongo_session *session) {
    mongo_ctx *mongo = session->mongo;
    // 一条连接同时只能有一个活跃事务;放第二个 session 进来会让后续写静默改跟它走
    if (NULL != mongo->session
        && session != mongo->session) {
        LOG_WARN("mongo connection already has an active transaction, begin rejected.");
        return ERR_FAILED;
    }
    session->txnnumber++;
    session->started = 0;
    FREE(session->options);//防止重复调用漏释放
    session->options = mongo_transaction_options(session, &session->optionslens);
    mongo->session = session;
    return ERR_OK;
}
int32_t mongo_commit(mongo_session *session, char *options, size_t optlens) {
    mongo_ctx *mongo = session->mongo;
    if (mongo->session != session) {
        LOG_WARN("mongo connection no longer bound to this session, commit rejected.");
        return ERR_FAILED;
    }
    int32_t flags = mongo_clear_flag(mongo);
    size_t lens;
    void *committransaction = mongo_pack_committransaction(session, options, optlens, &lens);
    mongo_set_flag(mongo, flags);
    mgopack_ctx *mgpack = _mongo_sendwait(mongo, committransaction, lens);
    if (NULL == mgpack) {
        return ERR_FAILED;
    }
    mongo->session = NULL;
    FREE(session->options);
    if (ERR_FAILED == mongo_parse_check_error(mgpack)) {
        return ERR_FAILED;
    }
    session->timeout = nowsec() + session->timeoutmin * 60;
    return ERR_OK;
}
int32_t mongo_rollback(mongo_session *session, char *options, size_t optlens) {
    mongo_ctx *mongo = session->mongo;
    if (mongo->session != session) {
        LOG_WARN("mongo connection no longer bound to this session, rollback rejected.");
        return ERR_FAILED;
    }
    int32_t flags = mongo_clear_flag(mongo);
    size_t lens;
    void *aborttransaction = mongo_pack_aborttransaction(session, options, optlens, &lens);
    mongo_set_flag(mongo, flags);
    mgopack_ctx *mgpack = _mongo_sendwait(mongo, aborttransaction, lens);
    if (NULL == mgpack) {
        return ERR_FAILED;
    }
    mongo->session = NULL;
    FREE(session->options);
    if (ERR_FAILED == mongo_parse_check_error(mgpack)) {
        return ERR_FAILED;
    }
    session->timeout = nowsec() + session->timeoutmin * 60;
    return ERR_OK;
}
int32_t kcp_synstart(task_ctx *task, struct kcp_ctx *kcp,
                     const char *ip, uint16_t port, const struct kcp_config *cfg) {
    // event 线程若拒绝建会话(conv 重复),此刻确定无存活会话:kcp_start 已隐式停掉先前的会话、本次的
    // 也没建起来,故置回"无会话"而非还原调用前的 sess/stopped。stopped 若留在 0,下次 kcp_synsend 会
    // 绕过守卫投到已消失的会话,被静默丢弃却返 ERR_OK,继而空等满一个 netread 超时;
    // maxpack 仍还原,保持"失败的调用不改动句柄"这一契约(send 已被 stopped 挡住,该值实际不可达)
    size_t prevmaxpack = kcp->maxpack;
    uint64_t sess = createid();
    if (ERR_OK != kcp_start(kcp, task->handle, sess, ip, port, cfg)) {
        return ERR_FAILED;
    }
    message_ctx *msg = _coro_wait(task, sess, MSG_TYPE_HANDSHAKED, task_get_netread_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        // 占位条目由随后到达的 CLOSE 清:会话已建立则 kcp_stop 发真 CLOSE,未建立则 _kcp_start 已补合成 CLOSE
        kcp_stop(kcp);
        LOG_WARN("task %s, kcp start timeout, skid %"PRIu64".", _NAME_OR(task->name), kcp->sk.skid);
        return ERR_FAILED;
    }
    if (MSG_TYPE_CLOSE == msg->mtype
        || ERR_OK != msg->erro) {
        kcp->sess = 0;
        kcp->stopped = 1;
        kcp->maxpack = prevmaxpack;
        return ERR_FAILED;
    }
    return ERR_OK;
}
void *kcp_synsend(task_ctx *task, struct kcp_ctx *kcp, void *data, size_t lens, int32_t copy, size_t *size) {
    if (0 == kcp->sess) {
        // sess==0 时 _coro_handle_recvfrom 内部恒新建协程,永远等不到本次唤醒
        CHECK_COPY_FREE(data, copy);
        return NULL;
    }
    if (ERR_OK != kcp_send(kcp, data, lens, copy)) {
        return NULL;
    }
    message_ctx *msg = _coro_wait(task, kcp->sess, MSG_TYPE_RECVFROM, task_get_netread_timeout(task));
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        // 不必清 sess:kcp_stop 置 stopped=1 后 kcp_send 首行即快速失败,不会再进 _coro_wait
        kcp_stop(kcp);
        LOG_WARN("task %s, kcp send timeout, skid %"PRIu64".", _NAME_OR(task->name), kcp->sk.skid);
        return NULL;
    }
    if (MSG_TYPE_CLOSE == msg->mtype) {
        // 会话已在 event 线程拆除(CLOSE 由 _kcp_notify_closed 发出)而 stopped 仍为 0,故须自行清 sess:
        // 否则下次 kcp_synsend 通过 0 == kcp->sess 守卫、kcp_send 投到已消失的会话被 _kcp_resolve
        // 静默丢弃却返 ERR_OK,继而空等满一个 netread 超时。与 Lua 侧 ctx:send 的同款分支对齐
        kcp->sess = 0;
        return NULL;
    }
    recvfrom_ctx *rfmsg = msg->data;
    SET_PTR(size, rfmsg->len);
    return rfmsg->data;
}
