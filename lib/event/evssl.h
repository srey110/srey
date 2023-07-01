#ifndef EVSSL_H_
#define EVSSL_H_

#include "macro.h"

#if WITH_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>

#define FREE_SSL(ssl)\
do {\
    if (NULL != ssl){\
        SSL_free(ssl); \
        ssl = NULL; \
    }\
}while (0)
typedef struct evssl_ctx evssl_ctx;

//SSL_FILETYPE_PEM SSL_FILETYPE_ASN1
evssl_ctx *evssl_new(const char *ca, const char *cert, const char *key, int32_t type, int32_t verify);
evssl_ctx *evssl_p12_new(const char *p12, const char *pwd, int32_t verify);
void evssl_free(evssl_ctx *evssl);

SSL *evssl_setfd(evssl_ctx *evssl, SOCKET fd);
int32_t evssl_tryacpt(SSL *ssl);
int32_t evssl_tryconn(SSL *ssl);

int32_t evssl_read(SSL *ssl, char *buf, size_t len, size_t *readed);
int32_t evssl_send(SSL *ssl, char *buf, size_t len, size_t *sended);

void evssl_shutdown(SSL *ssl, SOCKET fd);

#endif//WITH_SSL
#endif//EVSSL_H_
