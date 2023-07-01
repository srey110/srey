#include "event/evssl.h"
#if WITH_SSL
#include <openssl/pkcs12.h>

struct evssl_ctx {
    SSL_CTX *ssl;
};
static atomic_t _init_once = 0;
#define SSLCTX_ERRO()\
do {\
    unsigned long err = ERR_get_error();\
    LOG_WARN("errno: %lu, %s", err, ERR_error_string(err, NULL)); \
} while (0)

static inline void _ssl_options(evssl_ctx *evssl, int32_t verify) {
    SSL_CTX_set_options(evssl->ssl, SSL_OP_IGNORE_UNEXPECTED_EOF);//error:0A000126:SSL routines::unexpected eof while reading
    SSL_CTX_set_verify(evssl->ssl, verify, NULL);
    SSL_CTX_set_mode(evssl->ssl, SSL_MODE_AUTO_RETRY);
}
static inline evssl_ctx *_new_evssl(void) {
    if (ATOMIC_CAS(&_init_once, 0, 1)) {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        ERR_load_crypto_strings();
    }
    evssl_ctx *evssl;
    MALLOC(evssl, sizeof(evssl_ctx));
    ERR_clear_error();
    evssl->ssl = SSL_CTX_new(SSLv23_method());
    if (NULL == evssl->ssl) {
        FREE(evssl);
        SSLCTX_ERRO();
        return NULL;
    }
    SSL_CTX_set_security_level(evssl->ssl, 0);//ca md too weak
    return evssl;
}
evssl_ctx *evssl_new(const char *ca, const char *cert, const char *key, int32_t type, int32_t verify) {
    evssl_ctx *evssl = _new_evssl();
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
    _ssl_options(evssl, verify);
    return evssl;
}
evssl_ctx *evssl_p12_new(const char *p12, const char *pwd, int32_t verify) {
    evssl_ctx *evssl = _new_evssl();
    if (NULL == evssl) {
        return NULL;
    }
    if (EMPTYSTR(p12)) {
        _ssl_options(evssl, verify);
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
        PKCS12_free(pk12);
        evssl_free(evssl);
        return NULL;
    }
    if (1 != SSL_CTX_use_cert_and_key(evssl->ssl, cert, key, ca, 0)) {
        SSLCTX_ERRO();
        PKCS12_free(pk12);
        evssl_free(evssl);
        return NULL;
    }
    PKCS12_free(pk12);
    _ssl_options(evssl, verify);
    return evssl;
}
void evssl_free(evssl_ctx *evssl) {
    SSL_CTX_free(evssl->ssl);
    FREE(evssl);
}
SSL *evssl_setfd(evssl_ctx *evssl, SOCKET fd) {
    ERR_clear_error();
    SSL *ssl = SSL_new(evssl->ssl);
    if (NULL == ssl) {
        SSLCTX_ERRO();
        return NULL;
    }
    if (1 != SSL_set_fd(ssl, (int32_t)fd)) {
        SSL_free(ssl);
        return NULL;
    }
    return ssl;
}
int32_t evssl_tryacpt(SSL *ssl) {
    ERR_clear_error();
    int32_t rtn = SSL_accept(ssl);
    if (1 == rtn) {
        return 1;
    }
    if (SSL_ERROR_WANT_READ == SSL_get_error(ssl, rtn)) {
        return ERR_OK;
    }
    return ERR_FAILED;
}
int32_t evssl_tryconn(SSL *ssl) {
    ERR_clear_error();
    int32_t rtn = SSL_connect(ssl);
    if (1 == rtn) {
        return 1;
    }
    if (SSL_ERROR_WANT_READ == SSL_get_error(ssl, rtn)) {
        return ERR_OK;
    }
    return ERR_FAILED;
}
int32_t evssl_read(SSL *ssl, char *buf, size_t len, size_t *readed) {
    *readed = 0;
    int32_t rtn;
    ERR_clear_error();
    do {
        rtn = SSL_read(ssl, buf + *readed, (int32_t)(len - *readed));
        if (rtn > 0) {
            *readed += rtn;
        } else {
            if (0 == rtn) {
                return ERR_FAILED;
            }
            if (SSL_ERROR_WANT_READ == SSL_get_error(ssl, rtn)) {
                return ERR_OK;
            }
            return ERR_FAILED;
        }
    } while (*readed < len);
    return ERR_OK;
}
int32_t evssl_send(SSL *ssl, char *buf, size_t len, size_t *sended) {
    *sended = 0;
    int32_t rtn;
    ERR_clear_error();
    do {
        rtn = SSL_write(ssl, buf + *sended, (int32_t)(len - *sended));
        if (rtn > 0) {
            *sended += rtn;
        } else {
            if (SSL_ERROR_WANT_WRITE == SSL_get_error(ssl, rtn)) {
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
