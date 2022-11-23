#include "event/evssl.h"
#include "loger.h"
#if WITH_SSL
#include <openssl/provider.h>
#include <openssl/pkcs12.h>

#define EMPTY(str) ((NULL == str) || (0 == strlen(str)))
static volatile atomic_t _init_once = 0;
struct evssl_ctx
{
    SSL_CTX *ssl;
};

static inline void _init_ssl()
{
    if (ATOMIC_CAS(&_init_once, 0, 1))
    {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
    }
}
static inline struct evssl_ctx *_new_evssl(int32_t server, SSL_verify_cb v_cb)
{
    _init_ssl();
    struct evssl_ctx *evssl;
    MALLOC(evssl, sizeof(struct evssl_ctx));
    evssl->ssl = (server ? SSL_CTX_new(SSLv23_server_method()) : SSL_CTX_new(SSLv23_client_method()));
    ASSERTAB(NULL != evssl->ssl, SSL_ERR());
    SSL_CTX_set_security_level(evssl->ssl, 0);//ca md too weak
    if (NULL != v_cb)
    {
        SSL_CTX_set_verify(evssl->ssl, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, v_cb);
    }
    return evssl;
}
struct evssl_ctx *evssl_new(int32_t server, const char *ca, const char *cert, const char *key, int32_t type, SSL_verify_cb v_cb)
{
    if (server)
    {
        ASSERTAB(NULL != cert && NULL != key, "no have cert and key file.");
    }
    struct evssl_ctx *evssl = _new_evssl(server, v_cb);
    if (!EMPTY(ca))
    {
        ASSERTAB(1 == SSL_CTX_load_verify_locations(evssl->ssl, ca, NULL), SSL_ERR());
    }
    if (!EMPTY(cert))
    {
        ASSERTAB(1 == SSL_CTX_use_certificate_file(evssl->ssl, cert, type), SSL_ERR());
    }
    if (!EMPTY(key))
    {
        ASSERTAB(1 == SSL_CTX_use_PrivateKey_file(evssl->ssl, key, type), SSL_ERR());
        ASSERTAB(1 == SSL_CTX_check_private_key(evssl->ssl), SSL_ERR());
    }
    return evssl;
}
struct evssl_ctx *evssl_p12_new(int32_t server, const char *p12, const char *pwd, SSL_verify_cb v_cb)
{
    if (server)
    {
        ASSERTAB(NULL != p12, "no have p12 file.");
    }
    struct evssl_ctx *evssl = _new_evssl(server, v_cb);
    if (EMPTY(p12))
    {
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
void evssl_free(struct evssl_ctx *evssl)
{
    SSL_CTX_free(evssl->ssl);
    FREE(evssl);
}
SSL *evssl_setfd(struct evssl_ctx *evssl, SOCKET fd)
{
    SSL *ssl = SSL_new(evssl->ssl);
    if (NULL == ssl)
    {
        LOG_WARN("%s", SSL_ERR());
        return NULL;
    }
    if (1 != SSL_set_fd(ssl, (int32_t)fd))
    {
        SSL_free(ssl);
        return NULL;
    }
    return ssl;
}
int32_t evssl_tryacpt(SSL *ssl)
{
    int32_t rtn = SSL_accept(ssl);
    if (1 == rtn)
    {
        return 1;
    }
    if (SSL_ERROR_WANT_READ == SSL_get_error(ssl, rtn))
    {
        return ERR_OK;
    }
    return ERR_FAILED;
}
int32_t evssl_tryconn(SSL *ssl)
{
    int32_t rtn = SSL_connect(ssl);
    if (1 == rtn)
    {
        return 1;
    }
    if (SSL_ERROR_WANT_READ == SSL_get_error(ssl, rtn))
    {
        return ERR_OK;
    }
    return ERR_FAILED;
}
int32_t evssl_read(SSL *ssl, char *buf, size_t len)
{
    int32_t rtn, nread = 0;
    do
    {
        rtn = SSL_read(ssl, buf + nread, (int32_t)(len - nread));
        if (rtn > 0)
        {
            nread += rtn;
        }
        else
        {
            if (SSL_ERROR_WANT_READ == SSL_get_error(ssl, rtn))
            {
                break;
            }
            return ERR_FAILED;
        }
    } while (nread < len);
    return nread;
}
int32_t evssl_send(SSL *ssl, char *buf, size_t len)
{
    int32_t rtn, nsend = 0;
    do
    {
        rtn = SSL_write(ssl, buf + nsend, (int32_t)(len - nsend));
        if (rtn > 0)
        {
            nsend += rtn;
        }
        else
        {
            if (SSL_ERROR_WANT_WRITE == SSL_get_error(ssl, rtn))
            {
                break;
            }
            return ERR_FAILED;
        }
    } while (nsend < len);
    return nsend;
}
void evssl_shutdown(SSL *ssl, SOCKET fd)
{
    if (NULL != ssl)
    {
        SSL_shutdown(ssl);
    }
    else
    {
        shutdown(fd, SHUT_RD);
    }
}

#endif//WITH_SSL
