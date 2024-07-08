#ifndef EVSSL_H_
#define EVSSL_H_

#include "base/macro.h"
#if WITH_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "base/structs.h"

#define FREE_SSL(ssl) if (NULL != ssl){\
                          SSL_free(ssl); \
                          ssl = NULL; \
                      }
typedef struct evssl_ctx evssl_ctx;

void evssl_init(void);
//type:SSL_FILETYPE_PEM SSL_FILETYPE_ASN1    Ä¬ÈÏSSL_VERIFY_NONE
evssl_ctx *evssl_new(const char *ca, const char *cert, const char *key, int32_t type);
evssl_ctx *evssl_p12_new(const char *p12, const char *pwd);
//SSL_CTX_set_options  SSL_CTX_set_verify...
SSL_CTX *evssl_sslctx(evssl_ctx *evssl);
void evssl_free(evssl_ctx *evssl);

void evssl_pool_init(void);
void evssl_pool_free(void);
int32_t evssl_register(name_t name, evssl_ctx *evssl);
evssl_ctx *evssl_qury(name_t name);

SSL *evssl_setfd(evssl_ctx *evssl, SOCKET fd);
int32_t evssl_tryacpt(SSL *ssl);
int32_t evssl_tryconn(SSL *ssl);
int32_t evssl_read(SSL *ssl, char *buf, size_t len, size_t *readed);
int32_t evssl_send(SSL *ssl, char *buf, size_t len, size_t *sended);
void evssl_shutdown(SSL *ssl, SOCKET fd);

#endif//WITH_SSL
#endif//EVSSL_H_
