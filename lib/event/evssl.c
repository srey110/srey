#include "event/evssl.h"
#include "loger.h"
#if WITH_SSL
#include <openssl/pkcs12.h>

static atomic_t _init_once = 0;
struct evssl_ctx {
    SSL_CTX *ssl;
};

static inline void _init_ssl() {
    if (ATOMIC_CAS(&_init_once, 0, 1)) {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        ERR_load_crypto_strings();
    }
}
static inline evssl_ctx *_new_evssl(int32_t verify) {
    _init_ssl();
    evssl_ctx *evssl;
    MALLOC(evssl, sizeof(evssl_ctx));
    evssl->ssl = SSL_CTX_new(SSLv23_method());
    ASSERTAB(NULL != evssl->ssl, SSL_ERR());
    //fix error:0A000126:SSL routines::unexpected eof while reading https://github.com/openssl/openssl/issues/18866
    SSL_CTX_set_options(evssl->ssl, SSL_OP_IGNORE_UNEXPECTED_EOF);
    SSL_CTX_set_security_level(evssl->ssl, 0);//ca md too weak
    SSL_CTX_set_verify(evssl->ssl, verify, NULL);
    SSL_CTX_set_mode(evssl->ssl, SSL_MODE_AUTO_RETRY);
    return evssl;
}
evssl_ctx *evssl_new(const char *ca, const char *cert, const char *key, int32_t type, int32_t verify) {
    evssl_ctx *evssl = _new_evssl(verify);
    if (!EMPTYSTR(ca)) {
        ASSERTAB(1 == SSL_CTX_load_verify_locations(evssl->ssl, ca, NULL), SSL_ERR());
    }
    if (!EMPTYSTR(cert)) {
        ASSERTAB(1 == SSL_CTX_use_certificate_file(evssl->ssl, cert, type), SSL_ERR());
    }
    if (!EMPTYSTR(key)) {
        ASSERTAB(1 == SSL_CTX_use_PrivateKey_file(evssl->ssl, key, type), SSL_ERR());
        ASSERTAB(1 == SSL_CTX_check_private_key(evssl->ssl), SSL_ERR());
    }
    return evssl;
}
evssl_ctx *evssl_p12_new(const char *p12, const char *pwd, int32_t verify) {
    evssl_ctx *evssl = _new_evssl(verify);
    if (EMPTYSTR(p12)) {
        return evssl;
    }
    BIO *bio = BIO_new_file(p12, "rb");
    ASSERTAB(NULL != bio, SSL_ERR());
    PKCS12 *pk12 = d2i_PKCS12_bio(bio, NULL);
    BIO_free_all(bio);
    ASSERTAB(NULL != pk12, SSL_ERR());
    X509 *cert = NULL;
    EVP_PKEY *key = NULL;
    STACK_OF(X509) *ca = NULL;
    ASSERTAB(1 == PKCS12_parse(pk12, pwd, &key, &cert, &ca), SSL_ERR());
    ASSERTAB(1 == SSL_CTX_use_cert_and_key(evssl->ssl, cert, key, ca, 0), SSL_ERR());
    return evssl;
}
void evssl_free(evssl_ctx *evssl) {
    SSL_CTX_free(evssl->ssl);
    FREE(evssl);
}
SSL *evssl_setfd(evssl_ctx *evssl, SOCKET fd) {
    SSL *ssl = SSL_new(evssl->ssl);
    if (NULL == ssl) {
        LOG_ERROR("SSL_new failed.");
        return NULL;
    }
    if (1 != SSL_set_fd(ssl, (int32_t)fd)) {
        SSL_free(ssl);
        return NULL;
    }
    return ssl;
}
int32_t evssl_tryacpt(SSL *ssl) {
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
    int32_t rtn = SSL_connect(ssl);
    if (1 == rtn) {
        return 1;
    }
    if (SSL_ERROR_WANT_READ == SSL_get_error(ssl, rtn)) {
        return ERR_OK;
    }
    return ERR_FAILED;
}
int32_t evssl_read(SSL *ssl, char *buf, size_t len) {
    int32_t rtn, nread = 0;
    do {
        rtn = SSL_read(ssl, buf + nread, (int32_t)(len - nread));
        if (rtn > 0) {
            nread += rtn;
        } else {
            if (SSL_ERROR_WANT_READ == SSL_get_error(ssl, rtn)) {
                break;
            }
            return ERR_FAILED;
        }
    } while (nread < (int32_t)len);
    return nread;
}
int32_t evssl_send(SSL *ssl, char *buf, size_t len) {
    int32_t rtn, nsend = 0;
    do {
        rtn = SSL_write(ssl, buf + nsend, (int32_t)(len - nsend));
        if (rtn > 0) {
            nsend += rtn;
        } else {
            if (SSL_ERROR_WANT_WRITE == SSL_get_error(ssl, rtn)) {
                break;
            }
            return ERR_FAILED;
        }
    } while (nsend < (int32_t)len);
    return nsend;
}
void evssl_shutdown(SSL *ssl, SOCKET fd) {
    if (NULL != ssl) {
        SSL_shutdown(ssl);
    } else {
        shutdown(fd, SHUT_RD);
    }
}

#endif//WITH_SSL
