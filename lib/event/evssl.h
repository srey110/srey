#ifndef EVSSL_H_
#define EVSSL_H_

#include "base/macro.h"
#if WITH_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "base/structs.h"

#define FREE_SSL(ssl)\
    if (NULL != ssl){\
        SSL_free(ssl); \
        ssl = NULL; \
    }
typedef struct evssl_ctx evssl_ctx;
/// <summary>
/// 环境初始化
/// </summary>
void evssl_init(void);
/// <summary>
/// 加载ca cert key
/// </summary>
/// <param name="ca">ca文件, NULL 或 "" 不加载</param>
/// <param name="cert">cert文件, NULL 或 "" 不加载</param>
/// <param name="key">key文件, NULL 或 "" 不加载</param>
/// <param name="type">证书类型 SSL_FILETYPE_PEM,SSL_FILETYPE_ASN1</param>
/// <returns>evssl_ctx</returns>
evssl_ctx *evssl_new(const char *ca, const char *cert, const char *key, int32_t type);
/// <summary>
/// 加载p12证书
/// </summary>
/// <param name="p12">p12文件, NULL 或 "" 不加载</param>
/// <param name="pwd">证书密码</param>
/// <returns>evssl_ctx</returns>
evssl_ctx *evssl_p12_new(const char *p12, const char *pwd);
/// <summary>
/// 获取SSL_CTX,用于SSL_CTX_set_options SSL_CTX_set_verify等
/// </summary>
/// <param name="evssl">evssl_ctx</param>
/// <returns>SSL_CTX</returns>
SSL_CTX *evssl_sslctx(evssl_ctx *evssl);
/// <summary>
/// 释放evssl_ctx
/// </summary>
/// <param name="evssl">evssl_ctx</param>
void evssl_free(evssl_ctx *evssl);
/// <summary>
/// evssl_ctx池初始化
/// </summary>
void evssl_pool_init(void);
/// <summary>
/// evssl_ctx池释放
/// </summary>
void evssl_pool_free(void);
/// <summary>
/// 注册evssl_ctx
/// </summary>
/// <param name="name">名称</param>
/// <param name="evssl">evssl_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t evssl_register(name_t name, evssl_ctx *evssl);
/// <summary>
/// 根据名称获取evssl_ctx
/// </summary>
/// <param name="name">名称</param>
/// <returns>evssl_ctx  NULL失败</returns>
evssl_ctx *evssl_qury(name_t name);
/// <summary>
/// 设置socket为SSL链接
/// </summary>
/// <param name="evssl">evssl_ctx</param>
/// <param name="fd">socket句柄</param>
/// <returns>SSL  NULL失败</returns>
SSL *evssl_setfd(evssl_ctx *evssl, SOCKET fd);
/// <summary>
/// 开始服务端握手
/// </summary>
/// <param name="ssl">SSL</param>
/// <returns>1 成功 ERR_OK 需要更多数据 ERR_FAILED 失败</returns>
int32_t evssl_tryacpt(SSL *ssl);
/// <summary>
/// 开始客户端握手
/// </summary>
/// <param name="ssl">SSL</param>
/// <returns>1 成功 ERR_OK 需要更多数据 ERR_FAILED 失败</returns>
int32_t evssl_tryconn(SSL *ssl);
/// <summary>
/// 数据读取
/// </summary>
/// <param name="ssl">SSL</param>
/// <param name="buf">接收数据的buffer</param>
/// <param name="len">长度</param>
/// <param name="readed">读到的字节数</param>
/// <returns>ERR_OK 成功</returns>
int32_t evssl_read(SSL *ssl, char *buf, size_t len, size_t *readed);
/// <summary>
/// 数据写入
/// </summary>
/// <param name="ssl">SSL</param>
/// <param name="buf">要写入的数据</param>
/// <param name="len">长度</param>
/// <param name="sended">写入的字节数</param>
/// <returns>ERR_OK 成功</returns>
int32_t evssl_send(SSL *ssl, char *buf, size_t len, size_t *sended);
/// <summary>
/// shutdown
/// </summary>
/// <param name="ssl">SSL</param>
/// <param name="fd">socket句柄</param>
void evssl_shutdown(SSL *ssl, SOCKET fd);

#endif//WITH_SSL
#endif//EVSSL_H_
