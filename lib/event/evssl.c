#include "event/evssl.h"
#if WITH_SSL
#include "containers/sarray.h"
#include "thread/rwlock.h"
#include <openssl/pkcs12.h>

// ERR_error_string(err, NULL) 的 NULL 分支写 OpenSSL 进程级静态缓冲，多 watcher 线程并发失败时数据竞争；
// 改 ERR_error_string_n 写各自栈缓冲
#define SSLCTX_ERRO()\
    do {\
        unsigned long _sslec = ERR_get_error();\
        char _sslebuf[256];\
        ERR_error_string_n(_sslec, _sslebuf, sizeof(_sslebuf));\
        LOG_WARN("errno: %lu, %s", _sslec, _sslebuf);\
    } while (0)
// 每个 SSL_* 入口前都得保证错误队列是干净的,否则残留会被 _evssl_unexpected_eof 误判成截断。
// 队列空时 clear 仍要走一遍清理流程,比 peek 贵不少,而稳态下它恒为空,故先 peek 再决定
#define SSL_ERRQU_CLEAR()\
    do {\
        if (0 != ERR_peek_error()) {\
            ERR_clear_error();\
        }\
    } while (0)

// SSL上下文封装结构
struct evssl_ctx {
    SSL_CTX *ssl; // OpenSSL上下文
};
// SSL证书注册条目（名称 -> evssl_ctx 映射）
typedef struct certs_ctx {
    struct evssl_ctx *ssl;     // 对应的evssl_ctx
    char name[EVSSL_NAME_LEN]; // 注册名称
}certs_ctx;
ARR_DECL(certs_arr, certs_ctx)
static certs_arr *_arr_certs = NULL;// 全局证书注册池
static rwlock_ctx *_rwlck_certs = NULL;// 保护证书池的读写锁
static atomic_t _init_once = 0;// 保证证书池只初始化一次

// 设置SSL_CTX的通用选项：禁止重协商、不验证对端
// 不设 SSL_OP_IGNORE_UNEXPECTED_EOF：它把无 close_notify 的 EOF 伪装成 ZERO_RETURN，
// 关闭类型就分不出有序结束与截断。留着这条错误串是 _evssl_unexpected_eof 的判据，别再压掉
// 不设 SSL_MODE_AUTO_RETRY：该模式在非阻塞 socket 上会使 SSL_read/write 内部自旋，
// 阻塞 watcher 线程。WANT_READ/WANT_WRITE 由事件循环驱动重试。
// 设 SSL_MODE_RELEASE_BUFFERS：空闲连接交还读写缓冲(每条约 34KB)，代价是再收发时重新分配
// 设 SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER：合并写的缓冲在栈上，WANT_WRITE 后重试时地址会变，不开 OpenSSL 会判重试非法
// 写分片跟着 MAX_SSL_SEND_SIZE 收窄：_evpub_sock_send_ssl 本就把每次 SSL_write 卡在那个值上，
// 默认 16KB 的写缓冲永远填不满，收窄后每条连接省约 12.5KB 而行为不变。
// 超出 OpenSSL 允许的 512~16384 时该调用返 0，写缓冲退回默认值，只是省不到内存，不影响收发
static void _evssl_options(evssl_ctx *evssl) {
#ifdef SSL_OP_NO_RENEGOTIATION
    SSL_CTX_set_options(evssl->ssl, SSL_OP_NO_RENEGOTIATION);
#endif
    SSL_CTX_set_mode(evssl->ssl, SSL_MODE_RELEASE_BUFFERS | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_CTX_set_max_send_fragment(evssl->ssl, MAX_SSL_SEND_SIZE);
    SSL_CTX_set_verify(evssl->ssl, SSL_VERIFY_NONE, NULL);
}
void evssl_init(void) {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
}
// 分配并初始化一个新的evssl_ctx（使用TLS_method，安全级别设0）
static evssl_ctx *_evssl_new(void) {
    evssl_ctx *evssl;
    MALLOC(evssl, sizeof(evssl_ctx));
    SSL_ERRQU_CLEAR();
    evssl->ssl = SSL_CTX_new(TLS_method());
    if (NULL == evssl->ssl) {
        FREE(evssl);
        SSLCTX_ERRO();
        return NULL;
    }
    SSL_CTX_set_security_level(evssl->ssl, 0);//降到 0:有些 CA 证书的摘要算法(如 SHA1)在默认安全等级下会被拒
    return evssl;
}
evssl_ctx *evssl_new(const char *ca, const char *cert, const char *key, int32_t type) {
    evssl_ctx *evssl = _evssl_new();
    if (NULL == evssl) {
        return NULL;
    }
    if (!EMPTYSTR(ca)) {
        if (1 != SSL_CTX_load_verify_locations(evssl->ssl, ca, NULL)) {
            SSLCTX_ERRO();
            evssl_free(evssl);
            return NULL;
        }
    }
    if (!EMPTYSTR(cert)) {
        if (1 != SSL_CTX_use_certificate_file(evssl->ssl, cert, type)) {
            SSLCTX_ERRO();
            evssl_free(evssl);
            return NULL;
        }
    }
    if (!EMPTYSTR(key)) {
        if (1 != SSL_CTX_use_PrivateKey_file(evssl->ssl, key, type)) {
            SSLCTX_ERRO();
            evssl_free(evssl);
            return NULL;
        }
        if (1 != SSL_CTX_check_private_key(evssl->ssl)) {
            SSLCTX_ERRO();
            evssl_free(evssl);
            return NULL;
        }
    }
    _evssl_options(evssl);
    return evssl;
}
// PKCS12 解析产物的成套释放:四个对象必须一次全放,少一个就是每次加载 p12 泄漏一个 OpenSSL 对象
static void _p12_cleanup(X509 *cert, EVP_PKEY *key, STACK_OF(X509) *ca, PKCS12 *pk12) {
    X509_free(cert);
    EVP_PKEY_free(key);
    sk_X509_pop_free(ca, X509_free);
    PKCS12_free(pk12);
}
evssl_ctx *evssl_p12_new(const char *p12, const char *pwd) {
    evssl_ctx *evssl = _evssl_new();
    if (NULL == evssl) {
        return NULL;
    }
    if (EMPTYSTR(p12)) {
        _evssl_options(evssl);
        return evssl;
    }
    BIO *bio = BIO_new_file(p12, "rb");
    if (NULL == bio) {
        SSLCTX_ERRO();
        evssl_free(evssl);
        return NULL;
    }
    PKCS12 *pk12 = d2i_PKCS12_bio(bio, NULL);
    if (NULL == pk12) {
        SSLCTX_ERRO();
        BIO_free_all(bio);
        evssl_free(evssl);
        return NULL;
    }
    BIO_free_all(bio);
    X509 *cert = NULL;
    EVP_PKEY *key = NULL;
    STACK_OF(X509) *ca = NULL;
    if (1 != PKCS12_parse(pk12, pwd, &key, &cert, &ca)) {
        SSLCTX_ERRO();
        _p12_cleanup(cert, key, ca, pk12);
        evssl_free(evssl);
        return NULL;
    }
    if (1 != SSL_CTX_use_cert_and_key(evssl->ssl, cert, key, ca, 0)) {
        SSLCTX_ERRO();
        _p12_cleanup(cert, key, ca, pk12);
        evssl_free(evssl);
        return NULL;
    }
    _p12_cleanup(cert, key, ca, pk12);
    _evssl_options(evssl);
    return evssl;
}
SSL_CTX *evssl_sslctx(evssl_ctx *evssl) {
    return evssl->ssl;
}
void evssl_verify(evssl_ctx *evssl, int32_t mod, SSL_verify_cb vcb) {
    SSL_CTX_set_verify(evssl->ssl, mod, vcb);
}
void evssl_seclevel(evssl_ctx *evssl, int32_t level) {
    SSL_CTX_set_security_level(evssl->ssl, level);
}
int32_t evssl_min_proto(evssl_ctx *evssl, int32_t version) {
    return (1 == SSL_CTX_set_min_proto_version(evssl->ssl, version)) ? ERR_OK : ERR_FAILED;
}
void evssl_free(evssl_ctx *evssl) {
    SSL_CTX_free(evssl->ssl);
    FREE(evssl);
}
void evssl_pool_init(void) {
    if (ATOMIC_CAS(&_init_once, 0, 1)) {
        MALLOC(_rwlck_certs, sizeof(rwlock_ctx));
        MALLOC(_arr_certs, sizeof(certs_arr));
        rwlock_init(_rwlck_certs);
        certs_arr_init(_arr_certs, 0);
    }
}
void evssl_pool_free(void) {
    if (NULL == _arr_certs
        || NULL == _rwlck_certs) {
        return;
    }
    uint32_t n = certs_arr_size(_arr_certs);
    for (uint32_t i = 0; i < n; i++) {
        evssl_free(certs_arr_at(_arr_certs, (int32_t)i)->ssl);
    }
    certs_arr_free(_arr_certs);
    rwlock_free(_rwlck_certs);
    FREE(_arr_certs);
    FREE(_rwlck_certs);
    // 重置 _init_once，允许 evssl_pool_init 二次调用（嵌入式 loader_init/free 循环）
    ATOMIC_SET_RELEASE(&_init_once, 0);
}
// 在证书池中按名称查找certs_ctx（调用前须持有读锁）
static certs_ctx *_evssl_get(const char *name) {
    certs_ctx *cert;
    uint32_t n = certs_arr_size(_arr_certs);
    for (uint32_t i = 0; i < n; i++) {
        cert = certs_arr_at(_arr_certs, (int32_t)i);
        if (0 == strcmp(name, cert->name)) {
            return cert;
        }
    }
    return NULL;
}
int32_t evssl_register(const char *name, evssl_ctx *evssl) {
    if (NULL == evssl) {
        LOG_WARN("%s", ERRSTR_NULLP);
        return ERR_FAILED;
    }
    if (EMPTYSTR(name)) {
        LOG_WARN("%s", "ssl name empty.");
        evssl_free(evssl);
        return ERR_FAILED;
    }
    if (strlen(name) >= EVSSL_NAME_LEN) {
        LOG_ERROR("ssl name too long (>= %d): %s.", EVSSL_NAME_LEN, name);
        evssl_free(evssl);
        return ERR_FAILED;
    }
    if (NULL == _arr_certs
        || NULL == _rwlck_certs) {
        LOG_WARN("%s", "not call evssl_pool_init.");
        evssl_free(evssl);
        return ERR_FAILED;
    }
    certs_ctx cert;
    // 上面的 strlen(name) >= EVSSL_NAME_LEN 已挡过，装得下
    safe_fill_str(cert.name, sizeof(cert.name), name);
    cert.ssl = evssl;
    int32_t rtn;
    rwlock_wrlock(_rwlck_certs);
    if (NULL != _evssl_get(name)) {
        LOG_ERROR("ssl name %s repeat.", name);
        rtn = ERR_FAILED;
    } else {
        certs_arr_push_back(_arr_certs, &cert);
        rtn = ERR_OK;
    }
    rwlock_unlock(_rwlck_certs);
    if (ERR_OK != rtn) {
        evssl_free(evssl);
    }
    return rtn;
}
evssl_ctx *evssl_qury(const char *name) {
    //与 evssl_register 对称：池未 init 时返回 NULL 而非解引用 NULL 锁
    if (EMPTYSTR(name)
        || NULL == _arr_certs
        || NULL == _rwlck_certs) {
        return NULL;
    }
    certs_ctx *cert;
    evssl_ctx *ssl;
    rwlock_rdlock(_rwlck_certs);
    cert = _evssl_get(name);
    ssl = NULL == cert ? NULL : cert->ssl;
    rwlock_unlock(_rwlck_certs);
    return ssl;
}
SSL *evssl_setfd(evssl_ctx *evssl, SOCKET fd) {
    if ((uint64_t)fd > (uint64_t)INT_MAX) {
        LOG_ERROR("fd %llu exceeds INT_MAX, SSL_set_fd skipped.", (unsigned long long)fd);
        return NULL;
    }
    SSL_ERRQU_CLEAR();
    SSL *ssl = SSL_new(evssl->ssl);
    if (NULL == ssl) {
        SSLCTX_ERRO();
        return NULL;
    }
    if (1 != SSL_set_fd(ssl, (int32_t)fd)) {
        SSLCTX_ERRO();
        SSL_free(ssl);
        return NULL;
    }
    return ssl;
}
int32_t evssl_tryacpt(SSL *ssl) {
    SSL_ERRQU_CLEAR();
    int32_t rtn = SSL_accept(ssl);
    if (1 == rtn) {
        // 开预读省掉逐条记录各一次 recv。只能握手完成后设,不能提到 SSL_CTX 上——握手若由纯写
        // 就绪收尾,两个平台都不跑收取循环,CTX 级预读会把握手尾随的应用数据吸进 rbuf 搁住,
        // 要等下一次可读事件才被取走
        SSL_set_read_ahead(ssl, 1);
        return ERR_OK;
    }
    int32_t err = SSL_get_error(ssl, rtn);
    if (SSL_ERROR_WANT_READ == err) {
        return 1;
    }
    if (SSL_ERROR_WANT_WRITE == err) {
        return 2;
    }
    return ERR_FAILED;
}
int32_t evssl_tryconn(SSL *ssl) {
    SSL_ERRQU_CLEAR();
    int32_t rtn = SSL_connect(ssl);
    if (1 == rtn) {
        SSL_set_read_ahead(ssl, 1);// 理由见 evssl_tryacpt
        return ERR_OK;
    }
    int32_t err = SSL_get_error(ssl, rtn);
    if (SSL_ERROR_WANT_READ == err) {
        return 1;
    }
    if (SSL_ERROR_WANT_WRITE == err) {
        return 2;
    }
    return ERR_FAILED;
}
// 对端没发 close_notify 就断了。OpenSSL 3.0 起报成 SSL_ERROR_SSL 加这个 reason；
// 更早的版本与 LibreSSL 没有该 reason 码，那里一律当协议错，只是分不出截断。
// 判据取自错误队列，故每个 SSL_* 入口调用前都必须先清干净（见 SSL_ERRQU_CLEAR），残留会被误判成截断
static inline int32_t _evssl_unexpected_eof(int32_t err) {
#ifdef SSL_R_UNEXPECTED_EOF_WHILE_READING
    return (SSL_ERROR_SSL == err
        && SSL_R_UNEXPECTED_EOF_WHILE_READING == ERR_GET_REASON(ERR_peek_error())) ? 1 : 0;
#else
    (void)err;
    return 0;
#endif
}
int32_t evssl_read(SSL *ssl, char *buf, size_t len, size_t *readed) {
    *readed = 0;
    SSL_ERRQU_CLEAR();
    int32_t rtn = SSL_read(ssl, buf, len > INT32_MAX ? INT32_MAX : (int32_t)len);
    if (rtn > 0) {
        *readed = (size_t)rtn;
        return ERR_OK;
    }
    int32_t err = SSL_get_error(ssl, rtn);
    if (SSL_ERROR_ZERO_RETURN == err) {
        return 1;
    }
    /* post-handshake（TLS1.3 KeyUpdate）期间 SSL_read 也可能返回 WANT_WRITE，同等对待 */
    if (SSL_ERROR_WANT_READ == err
        || SSL_ERROR_WANT_WRITE == err) {
        return ERR_OK;
    }
    if (_evssl_unexpected_eof(err)) {
        return 2;
    }
    // 这一行是 SSL 错误在日志里的唯一出口：错误码不往上传，上层拿到 ERR_FAILED 直接关连接
    SSLCTX_ERRO();
    return ERR_FAILED;
}
int32_t evssl_send(SSL *ssl, char *buf, size_t len, size_t *sended) {
    *sended = 0;
    int32_t rtn;
    int32_t err;
    size_t remain;
    int32_t chunk;
    do {
        remain = len - *sended;
        chunk = remain > INT32_MAX ? INT32_MAX : (int32_t)remain;
        SSL_ERRQU_CLEAR();
        rtn = SSL_write(ssl, buf + *sended, chunk);
        if (rtn > 0) {
            *sended += rtn;
        } else {
            err = SSL_get_error(ssl, rtn);
            // 发送方向也可能先撞上 close_notify:post-handshake 期 SSL_write 会读入站记录。
            // 判据与返回码同 evssl_read
            if (SSL_ERROR_ZERO_RETURN == err) {
                return 1;
            }
            /* post-handshake（TLS1.3 KeyUpdate）期间 SSL_write 也可能返回 WANT_READ，同等对待 */
            if (SSL_ERROR_WANT_WRITE == err
                || SSL_ERROR_WANT_READ == err) {
                return ERR_OK;
            }
            if (_evssl_unexpected_eof(err)) {
                return 2;
            }
            SSLCTX_ERRO();
            return ERR_FAILED;
        }
    } while (*sended < len);
    return ERR_OK;
}
void evssl_shutdown(SSL *ssl, SOCKET fd, int32_t how) {
    if (NULL != ssl) {
        SSL_ERRQU_CLEAR();
        SSL_shutdown(ssl);
    }
    shutdown(fd, how);
}
int32_t evssl_version(SSL *ssl) {
    return SSL_version(ssl);
}
int32_t evssl_keyupdate(SSL *ssl, int32_t updatetype) {
    SSL_ERRQU_CLEAR();
    if (TLS1_3_VERSION != evssl_version(ssl)) {
        return ERR_FAILED;
    }
    if (1 != SSL_key_update(ssl, updatetype)) {
        SSLCTX_ERRO();
        return ERR_FAILED;
    }
    return ERR_OK;
}

#endif//WITH_SSL
