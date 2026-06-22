#include "event/evssl.h"
#if WITH_SSL
#include "containers/sarray.h"
#include "thread/rwlock.h"
#include <openssl/pkcs12.h>

#define SSLCTX_ERRO()\
    unsigned long err = ERR_get_error();\
    LOG_WARN("errno: %lu, %s", err, ERR_error_string(err, NULL))

// SSL上下文封装结构
struct evssl_ctx {
    SSL_CTX *ssl; // OpenSSL上下文
};
// SSL证书注册条目（名称 -> evssl_ctx 映射）
typedef struct certs_ctx {
    struct evssl_ctx *ssl;     // 对应的evssl_ctx
    char name[EVSSL_NAME_LEN]; // 注册名称
}certs_ctx;
static array_ctx *_arr_certs = NULL;       // 全局证书注册池（元素 certs_ctx）
static rwlock_ctx *_rwlck_certs = NULL;    // 保护证书池的读写锁
static atomic_t _init_once = 0;            // 保证证书池只初始化一次

// 设置SSL_CTX的通用选项：忽略意外EOF、不验证对端
// 不设 SSL_MODE_AUTO_RETRY：该模式在非阻塞 socket 上会使 SSL_read/write 内部自旋，
// 阻塞 watcher 线程。WANT_READ/WANT_WRITE 由事件循环驱动重试。
static void _evssl_options(evssl_ctx *evssl) {
    SSL_CTX_set_options(evssl->ssl, SSL_OP_IGNORE_UNEXPECTED_EOF); // 忽略: error:0A000126:SSL routines::unexpected eof while reading
    SSL_CTX_set_verify(evssl->ssl, SSL_VERIFY_NONE, NULL);
}
void evssl_init(void) {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
}
// 分配并初始化一个新的evssl_ctx（使用TLS_method，安全级别设0）
static evssl_ctx *_evssl_new(void) {
    evssl_ctx *evssl;
    MALLOC(evssl, sizeof(evssl_ctx));
    ERR_clear_error();
    evssl->ssl = SSL_CTX_new(TLS_method());
    if (NULL == evssl->ssl) {
        FREE(evssl);
        SSLCTX_ERRO();
        return NULL;
    }
    SSL_CTX_set_security_level(evssl->ssl, 0);//ca md too weak
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
        X509_free(cert);
        EVP_PKEY_free(key);
        sk_X509_pop_free(ca, X509_free);
        PKCS12_free(pk12);
        evssl_free(evssl);
        return NULL;
    }
    if (1 != SSL_CTX_use_cert_and_key(evssl->ssl, cert, key, ca, 0)) {
        SSLCTX_ERRO();
        X509_free(cert);
        EVP_PKEY_free(key);
        sk_X509_pop_free(ca, X509_free);
        PKCS12_free(pk12);
        evssl_free(evssl);
        return NULL;
    }
    X509_free(cert);
    EVP_PKEY_free(key);
    sk_X509_pop_free(ca, X509_free);
    PKCS12_free(pk12);
    _evssl_options(evssl);
    return evssl;
}
SSL_CTX *evssl_sslctx(evssl_ctx *evssl) {
    return evssl->ssl;
}
void evssl_verify(evssl_ctx *evssl, int32_t verify) {
    if (verify) {
        SSL_CTX_set_verify(evssl->ssl, SSL_VERIFY_PEER, NULL);
    } else {
        SSL_CTX_set_verify(evssl->ssl, SSL_VERIFY_NONE, NULL);
    }
}
void evssl_seclevel(evssl_ctx *evssl, int32_t level) {
    SSL_CTX_set_security_level(evssl->ssl, level);
}
void evssl_min_proto(evssl_ctx *evssl, int32_t version) {
    SSL_CTX_set_min_proto_version(evssl->ssl, version);
}
void evssl_free(evssl_ctx *evssl) {
    SSL_CTX_free(evssl->ssl);
    FREE(evssl);
}
void evssl_pool_init(void) {
    if (ATOMIC_CAS(&_init_once, 0, 1)) {
        MALLOC(_rwlck_certs, sizeof(rwlock_ctx));
        MALLOC(_arr_certs, sizeof(array_ctx));
        rwlock_init(_rwlck_certs);
        array_init(_arr_certs, sizeof(certs_ctx), 0);
    }
}
void evssl_pool_free(void) {
    if (NULL == _arr_certs
        || NULL == _rwlck_certs) {
        return;
    }
    uint32_t n = array_size(_arr_certs);
    for (uint32_t i = 0; i < n; i++) {
        evssl_free(((certs_ctx *)array_at(_arr_certs, i))->ssl);
    }
    array_free(_arr_certs);
    rwlock_free(_rwlck_certs);
    FREE(_arr_certs);
    FREE(_rwlck_certs);
    // 重置 _init_once，允许 evssl_pool_init 二次调用（嵌入式 loader_init/free 循环）
    ATOMIC_SET(&_init_once, 0);
}
// 在证书池中按名称查找certs_ctx（调用前须持有读锁）
static certs_ctx *_evssl_get(const char *name) {
    certs_ctx *cert;
    uint32_t n = array_size(_arr_certs);
    for (uint32_t i = 0; i < n; i++) {
        cert = array_at(_arr_certs, i);
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
    if (NULL == _arr_certs) {
        LOG_WARN("%s", "not call evssl_pool_init.");
        evssl_free(evssl);
        return ERR_FAILED;
    }
    certs_ctx cert;
    SNPRINTF(cert.name, sizeof(cert.name), "%s", name);
    cert.ssl = evssl;
    int32_t rtn;
    rwlock_wrlock(_rwlck_certs);
    if (NULL != _evssl_get(name)) {
        LOG_ERROR("ssl name %s repeat.", name);
        rtn = ERR_FAILED;
    } else {
        array_push_back(_arr_certs, &cert);
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
    rwlock_rdlock(_rwlck_certs);
    cert = _evssl_get(name);
    rwlock_unlock(_rwlck_certs);
    return NULL == cert ? NULL : cert->ssl;
}
SSL *evssl_setfd(evssl_ctx *evssl, SOCKET fd) {
    if ((uint64_t)fd > (uint64_t)INT_MAX) {
        LOG_ERROR("fd %llu exceeds INT_MAX, SSL_set_fd skipped.", (unsigned long long)fd);
        return NULL;
    }
    ERR_clear_error();
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
    ERR_clear_error();
    int32_t rtn = SSL_accept(ssl);
    if (1 == rtn) {
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
    ERR_clear_error();
    int32_t rtn = SSL_connect(ssl);
    if (1 == rtn) {
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
int32_t evssl_read(SSL *ssl, char *buf, size_t len, size_t *readed) {
    *readed = 0;
    ERR_clear_error();
    int32_t rtn = SSL_read(ssl, buf, len > INT32_MAX ? INT32_MAX : (int32_t)len);
    if (rtn > 0) {
        *readed = (size_t)rtn;
        return ERR_OK;
    }
    if (0 == rtn) {
        return ERR_FAILED;
    }
    int32_t err = SSL_get_error(ssl, rtn);
    /* TLS 重协商期间 SSL_read 也可能返回 WANT_WRITE，同等对待 */
    if (SSL_ERROR_WANT_READ == err
        || SSL_ERROR_WANT_WRITE == err) {
        return ERR_OK;
    }
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
        ERR_clear_error();
        rtn = SSL_write(ssl, buf + *sended, chunk);
        if (rtn > 0) {
            *sended += rtn;
        } else {
            //与 evssl_read 对称：rtn==0 通常表示连接已关闭，直接失败，省一次 SSL_get_error
            if (0 == rtn) {
                return ERR_FAILED;
            }
            err = SSL_get_error(ssl, rtn);
            /* TLS 重协商期间 SSL_write 也可能返回 WANT_READ，同等对待 */
            if (SSL_ERROR_WANT_WRITE == err
                || SSL_ERROR_WANT_READ == err) {
                return ERR_OK;
            }
            return ERR_FAILED;
        }
    } while (*sended < len);
    return ERR_OK;
}
void evssl_shutdown(SSL *ssl, SOCKET fd) {
    if (NULL != ssl) {
        ERR_clear_error();
        SSL_shutdown(ssl);
    } else {
        shutdown(fd, SHUT_RD);
    }
}

#endif//WITH_SSL
