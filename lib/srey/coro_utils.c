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

// 四家(mysql/pgsql/mongo/smtp)建链与断开骨架的回调:除 slot 与协议动作外四份代码逐字相同。
// 回调收 void* 而非强类型转:通过与原型不匹配的函数指针调用是 UB
typedef int32_t (*serial_conn_cb)(task_ctx *task, void *ctx);
typedef void (*serial_quit_cb)(void *ctx);
typedef int32_t (*serial_ping_cb)(void *ctx);
// 各 SMTP 命令认哪些应答码，集中一处好一眼看全。多码的只有 RCPT：
// 250 已接受、251 已接受但将转发（RFC 5321 §4.3.2），判 251 为失败会让 DATA 从不发出、整封信报错
static const char *const SMTP_CODE_OK[] = { "250" };// MAIL FROM / 正文 / RSET / NOOP
static const char *const SMTP_CODE_RCPT[] = { "250", "251" };
static const char *const SMTP_CODE_DATA[] = { "354" };
static const char *const SMTP_CODE_QUIT[] = { "221" };

static dns_ip *_dns_lookup_udp(task_ctx *task, const char *domain, int32_t ipv6, size_t *cnt, int32_t *nodata) {
    int32_t rtn;
    sock_ctx sk;
    const char *dnsip = dns_get_ip();
    if (ERR_OK == is_ipv6(dnsip)) {
        rtn = task_udp(task, PACK_NONE, "::", 0, &sk);
    } else {
        rtn = task_udp(task, PACK_NONE, "0.0.0.0", 0, &sk);
    }
    if (ERR_OK != rtn) {
        return NULL;
    }
    coro_sync(task, &sk);
    char buf[ONEK];
    uint16_t id;
    size_t lens = dns_request_pack(buf, domain, ipv6, &id);
    if (0 == lens) {
        ev_close(&task->loader->netev, &sk);
        return NULL;
    }
    void *resp = coro_sendto(task, &sk, dnsip, 53, buf, lens, &lens, 1);
    ev_close(&task->loader->netev, &sk);
    if (NULL == resp) {
        return NULL;
    }
    return dns_parse_pack(resp, lens, cnt, id, nodata);
}
static dns_ip *_dns_lookup_tcp(task_ctx *task, const char *domain, int32_t ipv6, size_t *cnt) {
    sock_ctx sk;
    const char *dnsip = dns_get_ip();
    if (ERR_OK != coro_connect(task, PACK_DNS, NULL, dnsip, 53, 0, NULL, &sk)) {
        return NULL;
    }
    char buf[ONEK];
    uint16_t id;
    size_t lens = dns_request_pack_tcp(buf, domain, ipv6, &id);
    if (0 == lens) {
        ev_close(&task->loader->netev, &sk);
        return NULL;
    }
    size_t rsize = 0;
    void *resp = coro_send(task, &sk, buf, lens, &rsize, 1);
    ev_close(&task->loader->netev, &sk);
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
    // 上面已挡过 host.lens >= hostlens - 7，装得下
    (void)copy_bounded(url->host.data, url->host.lens, host, hostlens, 1);
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
        if (ERR_OK != copy_bounded(host + 1, hlens - 2, ip, IP_LENS, 1)) {
            return ERR_FAILED;
        }
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
        // dns_ip.ip 也是 char[IP_LENS]，装得下
        (void)copy_bounded(ips[0].ip, strlen(ips[0].ip), ip, IP_LENS, 1);
        FREE(ips);
    } else {
        // is_ipaddr 已过，是 IP 字面量，装得下
        (void)copy_bounded(host, hlens, ip, IP_LENS, 1);
    }
    if (url->port.lens > 0) {
        // url_parse 只按冒号切分不校验字符,strtoul 会把 "80abc" 当 80 接受;
        // RFC 3986 §3.2.3 的 port 产生式只允许数字
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
static int32_t _ws_handshake(task_ctx *task, struct evssl_ctx *evssl, const char *ip, uint16_t port,
                             int32_t netev, const char *host, const char *uri, const char *secprot,
                             sock_ctx *sk, ws_secprots_ctx **spctx) {
    ws_hs_ctx *hsctx;
    char *reqpack = websock_pack_handshake(host, uri, secprot, &hsctx);
    if (NULL == reqpack) {
        return ERR_FAILED;
    }
    if (ERR_OK != coro_connect(task, PACK_WEBSOCK, evssl, ip, port, netev, hsctx, sk)) {
        FREE(reqpack);
        return ERR_FAILED;
    }
    if (ERR_OK != ev_send(&task->loader->netev, sk, reqpack, strlen(reqpack), 0)) {
        // 连接已建立而调用方只拿到失败,交不出连接也就补不了关闭,只能自己拆
        ev_close(&task->loader->netev, sk);
        return ERR_FAILED;
    }
    int32_t err;
    ws_secprots_ctx *sp = coro_handshaked(task, sk, &err, NULL);
    if (ERR_OK != err) {
        return ERR_FAILED;
    }
    SET_PTR(spctx, sp);
    return ERR_OK;
}
int32_t wbsock_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ws, const char *secprot,
    int32_t netev, sock_ctx *sk, ws_secprots_ctx **spctx) {
    SET_PTR(spctx, NULL);
    url_ctx url;
    int32_t iswss;
    char host[HOST_LENS + 8];// 主机名 + ":65535" + '\0'
    if (ERR_OK != _ws_parse_url(&url, ws, evssl, &iswss, host, sizeof(host))) {
        return ERR_FAILED;
    }
    char ip[IP_LENS];
    uint16_t port;
    if (ERR_OK != _ws_resolve_addr(task, &url, host, iswss, ip, &port)) {
        return ERR_FAILED;
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
    int32_t rtn = _ws_handshake(task, evssl, ip, port, netev, host, uribuf, secprot, sk, spctx);
    if (uribuf != uristack) {
        FREE(uribuf);
    }
    return rtn;
}
int32_t redis_connect(task_ctx *task, struct evssl_ctx *evssl, const char *ip, uint16_t port,
    const char *key, int32_t netev, sock_ctx *sk) {
    if (ERR_OK != coro_connect(task, PACK_REDIS, evssl, ip, port, netev, NULL, sk)) {
        return ERR_FAILED;
    }
    if (!EMPTYSTR(key)) {
        size_t size;
        char *auth = redis_pack(&size, "AUTH %s", key);
        if (NULL == auth) {
            ev_close(&task->loader->netev, sk);
            return ERR_FAILED;
        }
        redis_pack_ctx *rtn = coro_send(task, sk, auth, size, NULL, 0);
        if (NULL == rtn) {
            return ERR_FAILED;
        }
        if (RESP_STRING != rtn->prot
            || 2 != rtn->len
            || 0 != _memicmp(rtn->data, "ok", (size_t)rtn->len)) {
            ev_close(&task->loader->netev, sk);
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
static int32_t _mysql_do_connect(task_ctx *task, void *ctx) {
    mysql_ctx *mysql = (mysql_ctx *)ctx;
    if (ERR_OK != mysql_try_connect(task, mysql, 1)) {
        return ERR_FAILED;
    }
    if (ERR_OK != coro_wait_connect(task, &mysql->client.sk, mysql->client.evssl)) {
        return ERR_FAILED;
    }
    int32_t err;
    char *errmsg = coro_handshaked(task, &mysql->client.sk, &err, NULL);
    if (ERR_OK != err) {
        if (NULL != errmsg) {
            LOG_WARN("%s", errmsg);
        }
    }
    return err;
}
// 命令串行化的进出，成对使用：调用方进函数时先 held = xxx->serial 捏住指针，加锁解锁都用它。
// 捏指针而不是解锁时重读 xxx->serial：理由见 coro_serial_free 的调用方义务 2。
// held 为 NULL 表示这条连接不由 coro_utils 这套 API 管（经 *_try_connect 自行建立），不串行化，
// 但"必须在协程内"两档都成立，这一档得自己问。
// 失败（不在协程内 / 连接正在销毁）时调用方一律按失败返回，不得再调 unlock。
// mysql / pgsql / mongo / smtp 四家同一套语义，故只此一份
static int32_t _serial_lock(task_ctx *task, coro_serial_ctx *held) {
    if (NULL == held) {
        return 0 != coro_incoro(task) ? ERR_OK : ERR_FAILED;
    }
    return coro_serial_enter(held);
}
static void _serial_unlock(coro_serial_ctx *held) {
    if (NULL != held) {
        coro_serial_leave(held);
    }
}
// 拆掉执行器。置空必须排在 coro_serial_free 之后,且前后各认一次字段还是不是自己捏的那个——
// 认错就把别人新装的抹掉、连接从此一路无锁跑。两个时刻都会被换:进门前(我们排队等锁那段),
// free 之后(drain 就地 resume 让排队者当场跑业务代码,它可能 quit + connect 装上新的)
static void _serial_discard(coro_serial_ctx **slot, coro_serial_ctx *held) {
    if (held != *slot) {
        return;
    }
    coro_serial_free(held);
    if (held == *slot) {
        *slot = NULL;
    }
}
// 取执行器并上锁，字段空着就新建一个装上，owned 回带"是不是本次新建的"；
// 上锁失败返回 NULL，本次新建的那个已就地拆掉，调用方直接返回失败即可。
// serial 必须建在 try_connect 之前并立刻上锁:握手要多次挂起,中间放人进来就是往半成品连接上
// 发命令;try_connect 还会无条件覆写 sk.fd/skid,两个并发 connect 互相孤立对方的 socket。
// 重连路径(ping 内)进来时已持锁,按 ref 计数嵌套;serial 只在首次创建,重建会把排队者连同锁一起丢掉。
// owned 回带的用处:本次新建、握手又没成时连接不存在了,就地拆掉并把排队者唤醒成失败返回
static coro_serial_ctx *_serial_acquire(task_ctx *task, coro_serial_ctx **slot, int32_t *owned) {
    *owned = 0;
    if (NULL == *slot) {
        *slot = coro_serial_new(task);
        if (NULL == *slot) {
            return NULL;// 非 MCO task, 装不上执行器; 放行等于把受管连接静默降级成不串行
        }
        *owned = 1;
    }
    coro_serial_ctx *held = *slot;
    if (ERR_OK != _serial_lock(task, held)) {
        if (0 != *owned) {
            _serial_discard(slot, held);
        }
        return NULL;
    }
    return held;
}
// 建 serial 与整段握手都在锁内,理由见 _serial_acquire 上方。
// 只拆本次新建的:不是本次建的说明连接本来就在,拆了连累别人
// 票在 _serial_acquire(会 yield)之前采样,取 generation 而不是 skid:skid 在 try_connect 发起
// 连接时就已写好,采到的是在途那条,排队者醒来看不出变化。generation 在建连成功与断开时前进,
// 故"票变了"就是"排队期间有人动过这条连接";重入调用(如 selectdb 持锁再 connect)不 yield,
// 票必然没变,不会误短路。established 省不掉:quit 一样让代次前进,只看票会对着已关的连接报成功
static int32_t _serial_connect(task_ctx *task, coro_serial_ctx **slot, int32_t *established,
                               uint32_t *generation, serial_conn_cb doconn, void *ctx) {
    uint32_t ticket = *generation;
    int32_t owned;
    coro_serial_ctx *held = _serial_acquire(task, slot, &owned);
    if (NULL == held) {
        return ERR_FAILED;
    }
    // 排队期间别人已经建好了:再连一次会覆写 sk.fd/skid,把那条连接孤立到对端超时才回收
    if (ticket != *generation
        && 0 != *established) {
        _serial_unlock(held);
        return ERR_OK;
    }
    int32_t rtn = doconn(task, ctx);
    *established = (ERR_OK == rtn) ? 1 : 0;
    // 代次无条件前进:doconn 里的 *_try_connect 已经无条件覆写过 sk.fd/skid,成败与否这条连接
    // 的身份都换了。只在成功时前进,排队者会拿着旧票短路到一条已经不存在的连接上
    (*generation)++;
    if (ERR_OK != rtn
        && 0 != owned) {
        _serial_discard(slot, held);
    }
    _serial_unlock(held);
    return rtn;
}
// 摘指针 → 无执行器就直接断 → 上锁 → 断开动作 → 拆执行器 → 解锁。上锁是别把别人半途的等待拦腰打断;
// 拿不到锁分两档:别人已在销毁同一条连接就只拆不断(善后归先到方),不在协程内则先关 fd 再拆。
// established / generation 一律写在 doquit / _serial_discard 之前:这两个会就地 resume
// 排队者,它们可能当场重连,晚写就会盖掉新连接的状态。摘指针而不是解锁时重读,道理见 _serial_discard 上方
static void _serial_quit(task_ctx *task, coro_serial_ctx **slot, sock_ctx *sk, int32_t *established,
                         uint32_t *generation, serial_quit_cb doquit, void *ctx) {
    coro_serial_ctx *held = *slot;
    if (NULL == held) {
        // task 为 NULL 说明从未连接过(它由 *_try_connect 填),没有 socket 要断
        if (NULL == task) {
            return;
        }
        *established = 0;
        (*generation)++;
        // doquit 里的 coro_close / coro_send 只能在协程内调。非协程退到 ev_close:
        // 不等确认也不发协议层的 QUIT,但 fd 照样收回,不会漏到整个 task 拆除
        if (0 != coro_incoro(task)) {
            doquit(ctx);
            return;
        }
        LOG_WARN("quit outside coroutine context, close fd %d without protocol quit.", (int32_t)sk->fd);
        ev_close(&task->loader->netev, sk);
        return;
    }
    if (ERR_OK != _serial_lock(task, held)) {
        // 析构里调 *_quit 走的就是这里,不补 ev_close 会把 fd 漏到整个 task 拆除。
        // 整块排在 _serial_discard 之前,理由见函数上方
        if (NULL != task
            && 0 == coro_incoro(task)) {
            *established = 0;
            (*generation)++;
            LOG_WARN("quit outside coroutine context, close fd %d without protocol quit.", (int32_t)sk->fd);
            ev_close(&task->loader->netev, sk);
        }
        _serial_discard(slot, held);
        return;
    }
    *established = 0;
    (*generation)++;
    doquit(ctx);
    _serial_discard(slot, held);
    _serial_unlock(held);
}
// 探活失败即重连,held 非 NULL 时整段在锁内:理由同 _serial_acquire 上方。sk 传指针而不传值,
// 保证 fd/skid 在拿到锁之后才读——排队期间别人可能已经把连接重建过了。
// doconn 用 _X_do_connect 而不是公开的 X_connect:held 非 NULL 时两者只差一次可重入加解锁,
// 而 held 为 NULL(经 *_try_connect 自建)时 X_connect 会顺手装上执行器,把本不受管的连接变成
// 受管的;那一档本函数全程无锁、重连也不串行,与该类连接"不受管"的既有契约一致
static int32_t _serial_ping(coro_serial_ctx *held, task_ctx *task, sock_ctx *sk,
                           int32_t *established, uint32_t *generation,
                           serial_ping_cb doping, serial_conn_cb doconn, void *ctx) {
    if (ERR_OK != _serial_lock(task, held)) {
        return ERR_FAILED;
    }
    int32_t rtn = ERR_OK;
    if (ERR_OK != doping(ctx)) {
        *established = 0;
        coro_close(task, sk);
        rtn = doconn(task, ctx);
        *established = (ERR_OK == rtn) ? 1 : 0;
        // 这里换掉的 fd/skid 与 _serial_connect 里那次是同一件事,代次同样无条件前进,理由见那边
        (*generation)++;
    }
    _serial_unlock(held);
    return rtn;
}
int32_t mysql_connect(task_ctx *task, mysql_ctx *mysql) {
    return _serial_connect(task, &mysql->serial, &mysql->established, &mysql->generation,
                            _mysql_do_connect, mysql);
}
// 统一"发送+同步等待响应+校验 MPACK_OK"尾块;成功返回 ERR_OK,失败返回 ERR_FAILED
static int32_t _mysql_call(mysql_ctx *mysql, void *pack, size_t size) {
    mpack_ctx *mpack = coro_send(mysql->task, &mysql->client.sk, pack, size, NULL, 0);
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
    if (ERR_OK != _serial_lock(mysql->task, held)) {
        return ERR_FAILED;
    }
    int32_t rtn = _mysql_selectdb(mysql, database);
    _serial_unlock(held);
    return rtn;
}
// 向 MySQL 服务器发送 ping 包并等待响应，失败返回 ERR_FAILED
static int32_t _mysql_ping(void *ctx) {
    mysql_ctx *mysql = (mysql_ctx *)ctx;
    size_t size;
    void *ping = mysql_pack_ping(mysql, &size);
    return _mysql_call(mysql, ping, size);
}
int32_t mysql_ping(mysql_ctx *mysql) {
    return _serial_ping(mysql->serial, mysql->task, &mysql->client.sk,
                        &mysql->established, &mysql->generation,
                        _mysql_ping, _mysql_do_connect, mysql);
}
static mpack_ctx *_mysql_query(mysql_ctx *mysql, const char *sql, mysql_bind_ctx *mbind) {
    size_t size;
    void *query = mysql_pack_query(mysql, sql, mbind, &size);
    if (NULL == query) {
        return NULL;
    }
    return coro_send(mysql->task, &mysql->client.sk, query, size, NULL, 0);
}
// 逐个结果集回调,直到 more 为 0。四条约定:
// 1) more 在 cb 之前读——cb 里的 mysql_reader_init 会把 mpack->pack 摘走
// 2) cb 返回失败只记标志、剩余包照常排空:残留在连接缓冲里会让下一次查询 desync
// 3) 续读断连则无从排空,连接已废,直接失败返回
// 4) cb 为 NULL 只排空不回调(INSERT/UPDATE 用),且代调用方判一次 ERR 应答;有 cb 时不判,
//    ERR 包照样交给 cb、成败归 cb 说,否则"用 cb 自行处理服务端错误"的用法会被一律判失败
static int32_t _mysql_read_results(mysql_ctx *mysql, mpack_ctx *mpack, mysql_result_cb cb, void *udata) {
    int32_t failed = 0;
    int32_t more;
    for (;;) {
        more = mysql_more(mpack);
        if (NULL == cb) {
            // ERR 应答不断连(解析侧只置 pack_type 不置 PROT_ERROR),coro_send 照常返回一个
            // 合法 mpack,不判这一下主键冲突就会被当成功报回去
            if (MPACK_ERR == mpack->pack_type) {
                failed = 1;
            }
        } else if (ERR_OK != cb(mpack, udata)) {
            failed = 1;
        }
        if (0 == more) {
            break;
        }
        mpack = (mpack_ctx *)coro_recv(mysql->task, &mysql->client.sk, NULL);
        if (NULL == mpack) {
            return ERR_FAILED;
        }
    }
    return 0 != failed ? ERR_FAILED : ERR_OK;
}
int32_t mysql_query(mysql_ctx *mysql, const char *sql, mysql_bind_ctx *mbind,
                    mysql_result_cb cb, void *udata) {
    coro_serial_ctx *held = mysql->serial;
    if (ERR_OK != _serial_lock(mysql->task, held)) {
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
    mpack_ctx *mpack = coro_send(mysql->task, &mysql->client.sk, prepare, size, NULL, 0);
    return mysql_stmt_init(mpack);
}
mysql_stmt_ctx *mysql_stmt_prepare(mysql_ctx *mysql, const char *sql) {
    coro_serial_ctx *held = mysql->serial;
    if (ERR_OK != _serial_lock(mysql->task, held)) {
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
    return coro_send(stmt->mysql->task, &stmt->mysql->client.sk, exec, size, NULL, 0);
}
// stmt_id 是服务端按连接从 1 起分配的,重连后旧 id 发到新连接上,要么撞上那条连接里
// 恰好占用该 id 的语句、拿本次的参数去执行别人(静默返回错结果集),要么报 unknown handler。
// 判定必须在锁内做:排队期间连接可能已经被 *_ping 就地重连过。
// mysql_stmt_reset / _mysql_stmt_close 同此判定
int32_t mysql_stmt_execute(mysql_stmt_ctx *stmt, mysql_bind_ctx *mbind,
                           mysql_result_cb cb, void *udata) {
    mysql_ctx *mysql = stmt->mysql;
    coro_serial_ctx *held = mysql->serial;
    if (ERR_OK != _serial_lock(mysql->task, held)) {
        return ERR_FAILED;
    }
    if (sock_is_invalid(&mysql->client.sk)
        || stmt->skid != mysql->client.sk.skid) {
        _serial_unlock(held);
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
    mysql_ctx *mysql = stmt->mysql;
    coro_serial_ctx *held = mysql->serial;
    if (ERR_OK != _serial_lock(mysql->task, held)) {
        return ERR_FAILED;
    }
    if (sock_is_invalid(&mysql->client.sk)
        || stmt->skid != mysql->client.sk.skid) {
        _serial_unlock(held);
        return ERR_FAILED;
    }
    size_t size;
    void *resetpk = mysql_pack_stmt_reset(stmt, &size);
    int32_t rtn = _mysql_call(mysql, resetpk, size);
    _serial_unlock(held);
    return rtn;
}
// 组包发送并释放 stmt。fd == INVALID_SOCK 表示连接已关闭;skid 变了表示中途重连过——
// stmt_id 是服务端按连接分配的,旧 id 发到新连接上会把恰好占用该 id 的语句关掉。
// 两种情况都跳过发包,但本地照样释放
static void _mysql_stmt_close(mysql_stmt_ctx *stmt) {
    mysql_ctx *mysql = stmt->mysql;
    // 判权限必须排在组包之前:组包那一步会把连接的包序号清零,发不得的时候连这一步也不能做
    if (sock_is_invalid(&mysql->client.sk)
        || stmt->skid != mysql->client.sk.skid) {
        mysql_stmt_free(stmt);
        return;
    }
    size_t size;
    void *close = mysql_pack_stmt_close(stmt, &size);
    ev_send(&mysql->task->loader->netev, &mysql->client.sk, close, size, 0);
    mysql_stmt_free(stmt);
}
void mysql_stmt_close(mysql_stmt_ctx *stmt) {
    mysql_ctx *mysql = stmt->mysql;
    coro_serial_ctx *held = mysql->serial;
    // 两种"发不得"的情形都只做本地释放(服务端那份语句随连接关闭回收):插在别人结果集流中间
    // 违反半双工,而组包还会清连接的包序号。held 为 NULL 即这条连接不由本套 API 串行化
    // (经 *_try_connect 自建),此时发包会与网络线程的握手撞在同一个 mysql->id 上
    if (NULL == held
        || ERR_OK != coro_serial_enter(held)) {
        mysql_stmt_free(stmt);
        return;
    }
    _mysql_stmt_close(stmt);
    coro_serial_leave(held);
}
// 发 COM_QUIT 再关 socket；连接已关就什么都不发，执行器由 _serial_quit 那边拆
static void _mysql_do_quit(void *ctx) {
    mysql_ctx *mysql = (mysql_ctx *)ctx;
    if (sock_is_invalid(&mysql->client.sk)) {
        return;
    }
    size_t size;
    void *quit = mysql_pack_quit(&size);
    ev_send(&mysql->task->loader->netev, &mysql->client.sk, quit, size, 0);
    coro_close(mysql->task, &mysql->client.sk);
}
void mysql_quit(mysql_ctx *mysql) {
    _serial_quit(mysql->task, &mysql->serial, &mysql->client.sk, &mysql->established, &mysql->generation,
                 _mysql_do_quit, mysql);
}
static int32_t _smtp_do_connect(task_ctx *task, void *ctx) {
    smtp_ctx *smtp = (smtp_ctx *)ctx;
    if (ERR_OK != smtp_try_connect(task, smtp, 1)) {
        return ERR_FAILED;
    }
    if (ERR_OK != coro_wait_connect(task, &smtp->sk, smtp->evssl)) {
        return ERR_FAILED;
    }
    int32_t err;
    char *msg = (char *)coro_handshaked(task, &smtp->sk, &err, NULL);
    if (ERR_OK != err) {
        if (NULL != msg) {
            LOG_WARN("%s", msg);
        }
    }
    return err;
}
int32_t smtp_connect(task_ctx *task, smtp_ctx *smtp) {
    return _serial_connect(task, &smtp->serial, &smtp->established, &smtp->generation,
                            _smtp_do_connect, smtp);
}
// 统一 SMTP 的"组包 → 同步发送 → 校验应答码"三步:cmd 为 NULL(组包拒绝,如地址含 CRLF)
// 或没收到应答都返 ERR_FAILED。codes 传上面那几张表之一。
// cmd 是 format_va 的堆串,copy=0 把所有权交给 ev_send,成败都不用调用方释放
static int32_t _smtp_cmd(smtp_ctx *smtp, char *cmd, const char *const *codes, size_t ncode) {
    if (NULL == cmd) {
        return ERR_FAILED;
    }
    char *pack = coro_send(smtp->task, &smtp->sk, cmd, strlen(cmd), NULL, 0);
    if (NULL == pack) {
        return ERR_FAILED;
    }
    return smtp_check_codes(pack, codes, ncode);
}
// 发送 SMTP QUIT 命令并等待响应（不关闭 socket）
static void _smtp_quit(smtp_ctx *smtp) {
    _smtp_cmd(smtp, smtp_pack_quit(), SMTP_CODE_QUIT, ARRAY_SIZE(SMTP_CODE_QUIT));
}
// 发 QUIT 等 221 再关 socket；连接已关就什么都不发
static void _smtp_do_quit(void *ctx) {
    smtp_ctx *smtp = (smtp_ctx *)ctx;
    if (sock_is_invalid(&smtp->sk)) {
        return;
    }
    _smtp_quit(smtp);
    coro_close(smtp->task, &smtp->sk);
}
void smtp_quit(smtp_ctx *smtp) {
    _serial_quit(smtp->task, &smtp->serial, &smtp->sk, &smtp->established, &smtp->generation,
                 _smtp_do_quit, smtp);
}
// 发送 SMTP NOOP 命令检测连接是否存活，失败返回 ERR_FAILED
static int32_t _smtp_ping(void *ctx) {
    smtp_ctx *smtp = (smtp_ctx *)ctx;
    return _smtp_cmd(smtp, smtp_pack_ping(), SMTP_CODE_OK, ARRAY_SIZE(SMTP_CODE_OK));
}
int32_t smtp_ping(smtp_ctx *smtp) {
    return _serial_ping(smtp->serial, smtp->task, &smtp->sk,
                        &smtp->established, &smtp->generation,
                        _smtp_ping, _smtp_do_connect, smtp);
}
// 执行 SMTP 邮件发送流程（MAIL FROM → RCPT TO → DATA → 正文）。
// 发件人 / 收件人地址含 CRLF 时 smtp_pack_from / smtp_pack_rcpt 返 NULL，由 _smtp_cmd 拒发
static int32_t _smtp_send(smtp_ctx *smtp, mail_ctx *mail) {
    if (ERR_OK != _smtp_cmd(smtp, smtp_pack_from(mail->from.addr), SMTP_CODE_OK, ARRAY_SIZE(SMTP_CODE_OK))) {
        return ERR_FAILED;
    }
    uint32_t naddr = maddr_arr_size(&mail->addrs);
    mail_addr *addr;
    for (uint32_t i = 0; i < naddr; i++) {
        addr = maddr_arr_at(&mail->addrs, (int32_t)i);
        if (ERR_OK != _smtp_cmd(smtp, smtp_pack_rcpt(addr->addr), SMTP_CODE_RCPT, ARRAY_SIZE(SMTP_CODE_RCPT))) {
            return ERR_FAILED;
        }
    }
    if (ERR_OK != _smtp_cmd(smtp, smtp_pack_data(), SMTP_CODE_DATA, ARRAY_SIZE(SMTP_CODE_DATA))) {
        return ERR_FAILED;
    }
    return _smtp_cmd(smtp, mail_pack(mail), SMTP_CODE_OK, ARRAY_SIZE(SMTP_CODE_OK));
}
// 发送 SMTP RSET 命令重置会话状态（不关闭连接）
static int32_t _smtp_reset(smtp_ctx *smtp) {
    return _smtp_cmd(smtp, smtp_pack_reset(), SMTP_CODE_OK, ARRAY_SIZE(SMTP_CODE_OK));
}
int32_t smtp_send(smtp_ctx *smtp, mail_ctx *mail) {
    coro_serial_ctx *held = smtp->serial;
    if (ERR_OK != _serial_lock(smtp->task, held)) {
        return ERR_FAILED;
    }
    // 锁覆盖 _smtp_send + _smtp_reset 整段:RSET 清的是本次投递在服务端留下的会话状态,
    // 与发送是同一笔事。分开各包一次的话,别人的 MAIL FROM 会挤在中间被我们的 RSET 清掉
    int32_t rtn = _smtp_send(smtp, mail);
    // RSET 失败说明连接已经不干净,下一封信的 DATA 会被回 503,而 NOOP 仍答 250 让
    // smtp_ping 查不出来,只能就地关掉等重连。用 ev_close 不用 coro_close:不复用这条连接
    // 故不必等确认,而 RSET 失败常常正是对端已断,那条 CLOSE 已被取走,等就是持锁空等满超时
    if (ERR_OK != _smtp_reset(smtp)) {
        smtp->established = 0;// 明知已关就别留"还连着"的假值
        smtp->generation++;// 就地拆连接同样换了身份,理由同 _serial_quit
        ev_close(&smtp->task->loader->netev, &smtp->sk);
    }
    _serial_unlock(held);
    return rtn;
}
static int32_t _pgsql_do_connect(task_ctx *task, void *ctx) {
    pgsql_ctx *pg = (pgsql_ctx *)ctx;
    if (ERR_OK != pgsql_try_connect(task, pg, 1)) {
        return ERR_FAILED;
    }
    // pgsql SSL 是协议层收到服务端 'S' 应答后才发起(见 _pgsql_ssl_response)，此处不能传 pg->evssl，
    // 否则会等一个尚未触发的 SSLEXCHANGED 直到超时；coro_handshaked 的等待自然跨过该升级过程
    if (ERR_OK != coro_wait_connect(task, &pg->sk, NULL)) {
        return ERR_FAILED;
    }
    int32_t code;
    char *err = coro_handshaked(task, &pg->sk, &code, NULL);
    if (ERR_OK != code) {
        if (NULL != err) {
            LOG_WARN("%s", err);
        }
    }
    return code;
}
int32_t pgsql_connect(task_ctx *task, pgsql_ctx *pg) {
    return _serial_connect(task, &pg->serial, &pg->established, &pg->generation,
                            _pgsql_do_connect, pg);
}
int32_t pgsql_cancel(pgsql_ctx *pg) {
    if (sock_is_invalid(&pg->sk) || 0 == pg->pid) {
        return ERR_FAILED;
    }
    sock_ctx sk;
    // CancelRequest 须在独立 TCP 连接上发送，服务端处理后主动关闭连接，无任何响应
    if (ERR_OK != coro_connect(pg->task, PACK_NONE, NULL, pg->ip, pg->port, 0, NULL, &sk)) {
        return ERR_FAILED;
    }
    char buf[16];
    pgsql_pack_cancel(buf, pg->pid, pg->key);
    int32_t rtn = ev_send(&pg->task->loader->netev, &sk, buf, sizeof(buf), 1);
    ev_close(&pg->task->loader->netev, &sk);
    return rtn;
}
// 断开连接但不动 serial：selectdb 靠断连重连来换库,那期间锁还在本协程手上,
// 顺手把执行器销毁掉就把自己的锁毁了
static void _pgsql_disconnect(pgsql_ctx *pg) {
    if (sock_is_invalid(&pg->sk)) {
        return;
    }
    size_t lens;
    void *quit = pgsql_pack_terminate(&lens);
    ev_send(&pg->task->loader->netev, &pg->sk, quit, lens, 0);
    // 代次不在这里前进:两个调用方都已覆盖——_pgsql_do_quit 经 _serial_quit,
    // pgsql_selectdb 紧随的 pgsql_connect 无条件前进
    pg->established = 0;// selectdb 那条路径不经 _serial_quit,自己落
    coro_close(pg->task, &pg->sk);
}
// 断开动作与 selectdb 换库时用的是同一个,只是那边不动 serial,故 _pgsql_disconnect 保持强类型
static void _pgsql_do_quit(void *ctx) {
    _pgsql_disconnect((pgsql_ctx *)ctx);
}
void pgsql_quit(pgsql_ctx *pg) {
    _serial_quit(pg->task, &pg->serial, &pg->sk, &pg->established, &pg->generation,
                 _pgsql_do_quit, pg);
}
int32_t pgsql_selectdb(pgsql_ctx *pg, const char *database) {
    // 换库整段在锁内(含 set_db)：它改的 pg->database 是连接级状态,搁在锁外的话拿不到锁那次
    // 会留下"库名已换、连接还在旧库上",之后随便哪次 ping 重连就悄悄换了库
    coro_serial_ctx *held = pg->serial;
    if (ERR_OK != _serial_lock(pg->task, held)) {
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
    return coro_send(pg->task, &pg->sk, query, lens, NULL, 0);
}
// 必须正判 PGPACK_OK 而不是只判"收到了包":错位时读到的是上一条命令残留的包,当成自己的 pong
// 吃掉后,ping 这个唯一的重连判据就永远报健康。";" 回 EmptyQueryResponse,也归 PGPACK_OK
static int32_t _pgsql_ping(void *ctx) {
    pgsql_ctx *pg = (pgsql_ctx *)ctx;
    pgpack_ctx *pgpack = _pgsql_query(pg, ";");
    return (NULL == pgpack || PGPACK_OK != pgpack->type) ? ERR_FAILED : ERR_OK;
}
int32_t pgsql_ping(pgsql_ctx *pg) {
    return _serial_ping(pg->serial, pg->task, &pg->sk,
                        &pg->established, &pg->generation,
                        _pgsql_ping, _pgsql_do_connect, pg);
}
pgpack_ctx *pgsql_query(pgsql_ctx *pg, const char *sql) {
    coro_serial_ctx *held = pg->serial;
    if (ERR_OK != _serial_lock(pg->task, held)) {
        return NULL;
    }
    pgpack_ctx *rtn = _pgsql_query(pg, sql);
    _serial_unlock(held);
    return rtn;
}
static int32_t _pgsql_stmt_prepare(pgsql_ctx *pg, const char *name, const char *sql, int16_t nparam, uint32_t *oids) {
    size_t lens;
    void *parse = pgsql_pack_stmt_prepare(name, sql, nparam, oids, &lens);
    pgpack_ctx *pgpack = coro_send(pg->task, &pg->sk, parse, lens, NULL, 0);
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
    if (ERR_OK != _serial_lock(pg->task, held)) {
        return ERR_FAILED;
    }
    int32_t rtn = _pgsql_stmt_prepare(pg, name, sql, nparam, oids);
    _serial_unlock(held);
    return rtn;
}
pgpack_ctx *pgsql_stmt_execute(pgsql_ctx *pg, const char *name, pgsql_bind_ctx *bind, pgpack_format resultformat) {
    coro_serial_ctx *held = pg->serial;
    if (ERR_OK != _serial_lock(pg->task, held)) {
        return NULL;
    }
    size_t lens;
    void *exec = pgsql_pack_stmt_execute(name, bind, resultformat, &lens);
    if (NULL == exec) {
        _serial_unlock(held);
        return NULL;
    }
    pgpack_ctx *rtn = coro_send(pg->task, &pg->sk, exec, lens, NULL, 0);
    _serial_unlock(held);
    return rtn;
}
void pgsql_stmt_close(pgsql_ctx *pg, const char *name) {
    coro_serial_ctx *held = pg->serial;
    if (ERR_OK != _serial_lock(pg->task, held)) {
        return;// Close 也要等 CloseComplete,拿不到锁就别发,服务端那份随连接关闭一并回收
    }
    size_t lens;
    void *close = pgsql_pack_stmt_close(name, &lens);
    coro_send(pg->task, &pg->sk, close, lens, NULL, 0);
    _serial_unlock(held);
}
static pgpack_ctx *_pgsql_copy_in(pgsql_ctx *pg, const char *sql, const void *data, size_t lens) {
    // 第一步：发送 COPY SQL，等待服务端返回 CopyInResponse（PGPACK_COPY_IN）
    size_t qsize;
    void *query = pgsql_pack_query(sql, &qsize);
    pgpack_ctx *pgpack = coro_send(pg->task, &pg->sk, query, qsize, NULL, 0);
    // 不是 COPY_IN 就确实是服务端没进 COPY IN 模式(通常为 PGPACK_ERR)，直接交回调用方。
    // LISTEN 通知不会混进来:_pgsql_may_resume 对它返 ERR_FAILED，框架改走 recv 回调不唤醒等待者
    if (NULL == pgpack || PGPACK_COPY_IN != pgpack->type) {
        return pgpack;
    }
    // 第一次 coro_send 的返回值 pgpack 由框架在下次 yield 时经 _message_clean 自动释放，此处无需手动释放
    // 第二步：将 CopyData + CopyDone 合并为一个缓冲区，一次发送并等待 ReadyForQuery
    size_t dsize, csize;
    void *copy_data = pgsql_pack_copy_data(data, lens, &dsize);
    void *copy_done = pgsql_pack_copy_done(&csize);
    // 合并两段到连续缓冲区后发送，避免两次系统调用
    binary_ctx bwriter;
    binary_init_write(&bwriter, 0, 0);
    binary_set_binary(&bwriter, copy_data, dsize);
    binary_set_binary(&bwriter, copy_done, csize);
    FREE(copy_data);
    FREE(copy_done);
    return coro_send(pg->task, &pg->sk, bwriter.data, bwriter.offset, NULL, 0);
}
pgpack_ctx *pgsql_copy_in(pgsql_ctx *pg, const char *sql, const void *data, size_t lens) {
    // 本函数是两次往返:服务端进 COPY IN 模式后,在 CopyDone 之前它只认 CopyData/CopyFail,
    // 中间插进别人一条普通查询就整条连接报错。锁必须覆盖两次往返,不能各自包一次
    coro_serial_ctx *held = pg->serial;
    if (ERR_OK != _serial_lock(pg->task, held)) {
        return NULL;
    }
    pgpack_ctx *rtn = _pgsql_copy_in(pg, sql, data, lens);
    _serial_unlock(held);
    return rtn;
}
pgpack_ctx *pgsql_copy_out(pgsql_ctx *pg, const char *sql) {
    coro_serial_ctx *held = pg->serial;
    if (ERR_OK != _serial_lock(pg->task, held)) {
        return NULL;
    }
    // 发送 COPY SQL，解析器在收到所有 CopyData + CopyDone 后于 ReadyForQuery 时返回累积结果
    size_t qsize;
    void *query = pgsql_pack_query(sql, &qsize);
    pgpack_ctx *rtn = coro_send(pg->task, &pg->sk, query, qsize, NULL, 0);
    _serial_unlock(held);
    return rtn;
}
// mongo 没有 QUIT 命令，断连就是退出;不判 INVALID_SOCK 是因为 coro_close 对已关连接是空操作
static void _mongo_do_quit(void *ctx) {
    mongo_ctx *mongo = (mongo_ctx *)ctx;
    coro_close(mongo->task, &mongo->sk);
}
void mongo_quit(mongo_ctx *mongo) {
    _serial_quit(mongo->task, &mongo->serial, &mongo->sk, &mongo->established, &mongo->generation,
                 _mongo_do_quit, mongo);
}
// 执行 MongoDB SCRAM 认证流程（发送 client-first 消息并等待握手结果）
static int32_t _mongo_auth(mongo_ctx *mongo, const char *authmod) {
    if (ERR_OK != ev_ud_status(&mongo->task->loader->netev, &mongo->sk, mongo_status_auth())) {
        return ERR_FAILED;
    }
    size_t lens;
    void *client_first = mongo_pack_scram_client_first(mongo, authmod, &lens);
    if (NULL == client_first) {
        // scram 未初始化成功,回滚状态为 COMMAND,避免连接卡在 AUTH 态导致后续正常响应
        // 误入 _mongo_scram_auth 解引用 NULL scram(mongo.c 内已加判空兜底,此处是根因修复)
        ev_ud_status(&mongo->task->loader->netev, &mongo->sk, mongo_status_command());
        return ERR_FAILED;
    }
    if (ERR_OK != ev_send(&mongo->task->loader->netev, &mongo->sk, client_first, lens, 0)) {
        return ERR_FAILED;
    }
    int32_t err;
    coro_handshaked(mongo->task, &mongo->sk, &err, NULL);
    return err;
}
// 统一"组包判空 + 发送 + 同步等待响应"(不受 MORETOCOME 影响,总是等待),不校验命令级错误:
// 调用方各有各的用法(count 要 n 值、startsession 要 session、commit/rollback 要凭有无响应)。
// pack 为 NULL(组包被拒)与网络失败同样返回 NULL——两者在全部调用方那里处置相同。
// 串行化也落在这里(与 _mongo_send 两处覆盖全部命令站点):之后只做纯解析、不再有 I/O,
// 所以锁到本函数为止与锁整个命令函数等效。mongo_connect / mongo_ping 另有外层锁,靠 ref 嵌套
static mgopack_ctx *_mongo_sendwait(mongo_ctx *mongo, void *pack, size_t lens) {
    if (NULL == pack) {
        return NULL;
    }
    coro_serial_ctx *held = mongo->serial;
    if (ERR_OK != _serial_lock(mongo->task, held)) {
        FREE(pack);
        return NULL;
    }
    mgopack_ctx *rtn = coro_send(mongo->task, &mongo->sk, pack, lens, NULL, 0);
    if (NULL != rtn) {
        // 应答回来了就说明服务端处理过这条带 lsid 的命令,会话寿命已被延长
        mongo_session_touch(mongo);
    }
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
// hello + 认证：这两步与 TCP 建连合起来才算"一条可用的连接"，故收在连接入口，
// 首次建连与 ping 重连共用一份定义。未设用户名即视为免认证部署，只发 hello
static int32_t _mongo_handshake(mongo_ctx *mongo) {
    if (NULL == mongo_hello(mongo, NULL, 0)) {
        return ERR_FAILED;
    }
    if (0 == mongo->user[0]) {
        return ERR_OK;
    }
    // 认证多次往返且绕开 _mongo_call 那个漏斗,MORETOCOME 必须先摘掉
    int32_t flags = mongo_clear_flag(mongo);
    int32_t rtn = _mongo_auth(mongo, mongo->authmod);
    mongo_set_flag(mongo, flags);
    return rtn;
}
// 事务会话的绑定不能跨连接存活：新连接一建立就解绑，之后组包侧的 TRANSACTION_OPTIONS 才不会
// 把上一代的 lsid/txnNumber 附到 hello 及后续命令上。
// 清在这里而不是断开时的 _mongo_udfree：那个回调跑在网络线程，而组包侧是在属主线程上
// 判 mongo->session 非空后解引用它的 options/started，跨线程置空会让那两步之间读到 NULL。
// 放在连接入口还顺带覆盖"在一条仍打开的连接上重入 connect"——那种情况根本不会触发 udfree
static int32_t _mongo_do_connect(task_ctx *task, void *ctx) {
    mongo_ctx *mongo = (mongo_ctx *)ctx;
    if (ERR_OK != mongo_try_connect(task, mongo, 1)) {
        return ERR_FAILED;
    }
    mongo_clear_session(mongo);
    if (ERR_OK != coro_wait_connect(task, &mongo->sk, mongo->evssl)) {
        return ERR_FAILED;
    }
    if (ERR_OK != _mongo_handshake(mongo)) {
        // 握手没成必须自己拆:留下的是一条活着却没认证的连接,而 ping 发的 {ping:1} 免认证,
        // 会一直报健康、永不重连。现取 sk:对端已断时它已被 teardown 复位成 INVALID_SOCK
        coro_close(task, &mongo->sk);
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t mongo_connect(task_ctx *task, mongo_ctx *mongo) {
    return _serial_connect(task, &mongo->serial, &mongo->established, &mongo->generation,
                            _mongo_do_connect, mongo);
}
static int32_t _mongo_ping(void *ctx) {
    mongo_ctx *mongo = (mongo_ctx *)ctx;
    int32_t flags = mongo_clear_flag(mongo);
    size_t lens;
    void *ping = mongo_pack_ping(mongo, &lens);
    mongo_set_flag(mongo, flags);
    return NULL == _mongo_call(mongo, ping, lens) ? ERR_FAILED : ERR_OK;
}
int32_t mongo_ping(mongo_ctx *mongo) {
    return _serial_ping(mongo->serial, mongo->task, &mongo->sk,
                        &mongo->established, &mongo->generation,
                        _mongo_ping, _mongo_do_connect, mongo);
}
// MongoDB 统一发送函数：设置了 MORETOCOME 标志时仅发送不等待响应，否则同步等待响应。
// pack 为 NULL(组包被拒)在此一并吸收,语义同 _mongo_call
// MORETOCOME 变体:置位时只发不等。同样在锁内发——不等响应也不能乱序,
// 后面那条 find 得看得见前面这批 insert。
// 判定问包不问 mongo->flags(加锁会挂起),理由见 mongo_pack_check_flag
static int32_t _mongo_send(mongo_ctx *mongo, void *pack, size_t lens, mgopack_ctx **mgopack) {
    if (NULL == pack) {
        return ERR_FAILED;
    }
    coro_serial_ctx *held = mongo->serial;
    if (ERR_OK != _serial_lock(mongo->task, held)) {
        FREE(pack);
        return ERR_FAILED;
    }
    int32_t rtn = ERR_FAILED;
    if (mongo_pack_check_flag(pack, MORETOCOME)) {
        rtn = ev_send(&mongo->task->loader->netev, &mongo->sk, pack, lens, 0);
    } else {
        mgopack_ctx *rtnpack = coro_send(mongo->task, &mongo->sk, pack, lens, NULL, 0);
        if (NULL != rtnpack) {
            SET_PTR(mgopack, rtnpack);
            rtn = ERR_OK;
            // 写命令同样带 lsid,续期口径与 _mongo_sendwait 一致。
            // MORETOCOME 那支没有应答,不续
            mongo_session_touch(mongo);
        }
    }
    _serial_unlock(held);
    return rtn;
}
// 发命令并取校验结果:>=0 是服务端回的 n(insert/update/delete 的返回值就是它),<0 失败;
// MORETOCOME 下没有响应,按成功计返回 0。不需要 n 的命令在调用点自己压平成 ERR_OK
static int32_t _mongo_send_checked(mongo_ctx *mongo, void *pack, size_t lens) {
    mgopack_ctx *mgpack = NULL;
    if (ERR_OK != _mongo_send(mongo, pack, lens, &mgpack)) {
        return ERR_FAILED;
    }
    if (NULL == mgpack) {
        return ERR_OK;
    }
    return mongo_parse_check_error(mgpack);
}
// 同 _mongo_send_checked,只是把 pack 本身交出去:发送失败 / MORETOCOME 无响应 / 命令级错误都返 NULL
static mgopack_ctx *_mongo_send_pack(mongo_ctx *mongo, void *pack, size_t lens) {
    mgopack_ctx *mgpack = NULL;
    if (ERR_OK != _mongo_send(mongo, pack, lens, &mgpack)) {
        return NULL;
    }
    if (NULL == mgpack) {
        return NULL;
    }
    return (ERR_FAILED == mongo_parse_check_error(mgpack)) ? NULL : mgpack;
}
int32_t mongo_drop(mongo_ctx *mongo, char *options, size_t optlens) {
    size_t lens;
    void *drop = mongo_pack_drop(mongo, options, optlens, &lens);
    return _mongo_send_checked(mongo, drop, lens) < 0 ? ERR_FAILED : ERR_OK;
}
int32_t mongo_insert(mongo_ctx *mongo, char *docs, size_t dlens, char *options, size_t optlens) {
    size_t lens;
    void *insert = mongo_pack_insert(mongo, docs, dlens, options, optlens, &lens);
    return _mongo_send_checked(mongo, insert, lens);
}
int32_t mongo_update(mongo_ctx *mongo, char *updates, size_t ulens, char *options, size_t optlens) {
    size_t lens;
    void *update = mongo_pack_update(mongo, updates, ulens, options, optlens, &lens);
    return _mongo_send_checked(mongo, update, lens);
}
int32_t mongo_delete(mongo_ctx *mongo, char *deletes, size_t dlens, char *options, size_t optlens) {
    size_t lens;
    void *del = mongo_pack_delete(mongo, deletes, dlens, options, optlens, &lens);
    return _mongo_send_checked(mongo, del, lens);
}
mgopack_ctx *mongo_bulkwrite(mongo_ctx *mongo, char *ops, size_t olens, char *nsinfo, size_t nlens, char *options, size_t optlens) {
    size_t lens;
    void *bulkwrite = mongo_pack_bulkwrite(mongo, ops, olens, nsinfo, nlens, options, optlens, &lens);
    return _mongo_send_pack(mongo, bulkwrite, lens);
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
    return _mongo_send_pack(mongo, killcursors, lens);
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
    return _mongo_send_checked(mongo, createindexes, lens) < 0 ? ERR_FAILED : ERR_OK;
}
int32_t mongo_dropindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t optlens) {
    size_t lens;
    void *dropindexes = mongo_pack_dropindexes(mongo, indexes, ilens, options, optlens, &lens);
    return _mongo_send_checked(mongo, dropindexes, lens) < 0 ? ERR_FAILED : ERR_OK;
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
    mongo_session_renew(session);
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
    // 不经 mongo_session_touch：refresh 允许在事务外调，那时连接并没绑这个 session
    mongo_session_renew(session);
    return ERR_OK;
}
void mongo_freesession(mongo_session *session) {
    mongo_ctx *mongo = session->mongo;
    // 解绑必须排在会挂起的 _mongo_send 之前:挂起窗口里 mongo->session 还指着本 session,
    // 并发协程的 commit/rollback 就能过 _mongo_txn_end 的绑定判定,醒来后写进已释放的 session
    if (mongo->session == session) {
        mongo->session = NULL;
    }
    // 不看绑定:endsession 只按 session->uuid 组包,不带连接当前绑定的事务上下文。
    // 也不看连接换没换过:服务端的会话记录不随连接消失,漏发这一包就要挂到会话超时才回收
    size_t lens;
    void *endsession = mongo_pack_endsession(session, &lens);
    // 释放必须排在 _mongo_send 之后:它取的 serial 锁把本函数排在在途 commit/rollback 之后,
    // 那边醒来还要读 session->options。提前释放就是让它读已释放内存
    _mongo_send(mongo, endsession, lens, NULL);
    FREE(session->options);
    FREE(session);
}
// 事务绑定规则:组包一律从 mongo->session 取事务上下文,调用方手上的 session 必须就是连接
// 当前绑定的那个。begin 靠拒绝第二个 session 维持;commit/rollback 的判定在两个 packer 里
// (_mongo_txn_bound),分叉时组包返 NULL,发送侧照常按失败走。
// 重连只废掉在途事务不废会话(会话按 lsid 记在服务端、与连接无关),故此处不认代次只认绑定。
// 早退不释放 session->options 不算漏:重新 begin 与 mongo_freesession 都会释放。
// commit/rollback 各套一层外锁,组包发送同在锁内;begin 不挂起,不套锁
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
// 事务收尾:commit 与 rollback 只差组包函数。清绑定(mongo->session = NULL)必须排在
// check_error 之前——服务端已经收下了,本地就不能再认为事务在跑,否则同一 session 还能再收尾一次
static int32_t _mongo_txn_end(mongo_session *session, char *options, size_t optlens,
                              void *(*pack)(mongo_session *, char *, size_t, size_t *)) {
    mongo_ctx *mongo = session->mongo;
    int32_t flags = mongo_clear_flag(mongo);
    size_t lens;
    void *txnpack = pack(session, options, optlens, &lens);
    mongo_set_flag(mongo, flags);
    mgopack_ctx *mgpack = _mongo_sendwait(mongo, txnpack, lens);
    if (NULL == mgpack) {
        return ERR_FAILED;
    }
    mongo->session = NULL;
    FREE(session->options);
    if (ERR_FAILED == mongo_parse_check_error(mgpack)) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t mongo_commit(mongo_session *session, char *options, size_t optlens) {
    coro_serial_ctx *held = session->mongo->serial;
    if (ERR_OK != _serial_lock(session->mongo->task, held)) {
        return ERR_FAILED;
    }
    int32_t rtn = _mongo_txn_end(session, options, optlens, mongo_pack_committransaction);
    _serial_unlock(held);
    return rtn;
}
int32_t mongo_rollback(mongo_session *session, char *options, size_t optlens) {
    coro_serial_ctx *held = session->mongo->serial;
    if (ERR_OK != _serial_lock(session->mongo->task, held)) {
        return ERR_FAILED;
    }
    int32_t rtn = _mongo_txn_end(session, options, optlens, mongo_pack_aborttransaction);
    _serial_unlock(held);
    return rtn;
}
int32_t kcp_synstart(task_ctx *task, struct kcp_ctx *kcp,
                     const char *ip, uint16_t port, const struct kcp_config *cfg) {
    // event 线程拒绝建会话(conv 重复)时确定无存活会话,故置回"无会话"而非还原调用前的
    // sess/stopped——stopped 留在 0 会让下次 kcp_synsend 绕过守卫投到已消失的会话;
    // maxpack 仍还原,保持"失败的调用不改动句柄"
    size_t prevmaxpack = kcp->maxpack;
    uint64_t sess = createid();
    if (ERR_OK != kcp_start(kcp, task->handle, sess, ip, port, cfg)) {
        return ERR_FAILED;
    }
    message_ctx *msg = _coro_wait(task, sess, MSG_TYPE_HANDSHAKED, task_get_netread_timeout(task));
    // 失败分支动 kcp 之前先认一次 sess,理由同 kcp_synsend:换掉之后那三个字段属于新会话,
    // 抹了它既发不出也停不掉。prevmaxpack 同理只对自己这次调用有意义
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        // 只为停掉会话本身,不然它留在 event 线程的会话表里；mapco 条目不用管,
        // 超时路径的 _coro_timeout_monitor 摘掉等待者后已经删过了
        if (sess == kcp->sess) {
            kcp_stop(kcp);
        }
        LOG_WARN("task %s, kcp start timeout, skid %"PRIu64".", _NAME_OR(task->name), kcp->sk.skid);
        return ERR_FAILED;
    }
    if (MSG_TYPE_CLOSE == msg->mtype
        || ERR_OK != msg->erro) {
        if (sess == kcp->sess) {
            kcp->sess = 0;
            kcp->stopped = 1;
            kcp->maxpack = prevmaxpack;
        }
        return ERR_FAILED;
    }
    return ERR_OK;
}
void *kcp_synsend(task_ctx *task, struct kcp_ctx *kcp, void *data, size_t lens, int32_t copy, size_t *size) {
    // 捏住 sess 而不是等待与判定时各读一次 kcp->sess:挂起期间它可能被别的协程换掉,
    // 那时按新值等待就等错了会话,按新值回写更会抹掉别人刚建好的那条
    uint64_t sess = kcp->sess;
    if (0 == sess) {
        // sess==0 时 RECVFROM 的分发(_coro_handle_miss_create)内部恒新建协程,永远等不到本次唤醒
        CHECK_COPY_FREE(data, copy);
        return NULL;
    }
    if (ERR_OK != kcp_send(kcp, data, lens, copy)) {
        return NULL;
    }
    message_ctx *msg = _coro_wait(task, sess, MSG_TYPE_RECVFROM, task_get_netread_timeout(task));
    // 两个失败分支都先认一次 sess:CLOSE 是按 sess 广播给该 sess 下全部等待者的(不分 mtype),
    // 而醒来时 kcp->sess 可能已被同 task 另一协程 stop + 重启换成新会话,那时动 kcp 就是打在新会话上
    if (MSG_TYPE_TIMEOUT == msg->mtype) {
        if (sess == kcp->sess) {
            kcp_stop(kcp);// 置 stopped=1 后 kcp_send 首行即快速失败,不必再清 sess
        }
        LOG_WARN("task %s, kcp send timeout, skid %"PRIu64".", _NAME_OR(task->name), kcp->sk.skid);
        return NULL;
    }
    if (MSG_TYPE_CLOSE == msg->mtype) {
        // 会话已在 event 线程拆除(CLOSE 由 _kcp_notify_closed 发出)而 stopped 仍为 0,故须自行清 sess:
        // 否则下次 kcp_synsend 通过 0 == kcp->sess 守卫、kcp_send 投到已消失的会话被静默丢弃却返
        // ERR_OK,继而空等满一个 netread 超时
        if (sess == kcp->sess) {
            kcp->sess = 0;
        }
        return NULL;
    }
    recvfrom_ctx *rfmsg = msg->data;
    SET_PTR(size, rfmsg->len);
    return rfmsg->data;
}
