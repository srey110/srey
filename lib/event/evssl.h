#ifndef EVSSL_H_
#define EVSSL_H_

#include "base/macro.h"

#define EVSSL_NAME_LEN 64

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
/// 加载ca cert key 创建 SSL 上下文。
/// 默认 SSL_VERIFY_NONE（不验证对端）+ security_level=0；适合内网/自签证书场景。
/// 公网客户端连 trusted CA 时务必显式：
///   evssl_verify(ssl, SSL_VERIFY_PEER, NULL)  启用对端证书验证(第三参为自定义校验回调,不用就传 NULL)
///   evssl_seclevel(ssl, 2)               禁用 RSA<2048 / MD5 等弱算法
///   evssl_min_proto(ssl, TLS1_2_VERSION) 禁用 TLS 小于 1.2
/// </summary>
/// <param name="ca">ca文件, NULL 或 "" 不加载。这是验对端用的信任库，不会被当成自己要发出的证书链</param>
/// <param name="cert">cert文件, NULL 或 "" 不加载。只装这一张叶证书，不支持证书链——
///   由中间 CA 签发的证书（Let's Encrypt 之类）握手时只发叶证书，对端补不上链会验不过</param>
/// <param name="key">key文件, NULL 或 "" 不加载</param>
/// <param name="type">证书类型 SSL_FILETYPE_PEM,SSL_FILETYPE_ASN1</param>
/// <returns>evssl_ctx</returns>
evssl_ctx *evssl_new(const char *ca, const char *cert, const char *key, int32_t type);
/// <summary>
/// 加载p12证书。
/// 默认安全选项与 evssl_new 一致（SSL_VERIFY_NONE / security_level=0）；
/// 公网客户端务必配合 evssl_verify / evssl_seclevel / evssl_min_proto 显式加固。
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
/// 设置是否验证对端证书（需先加载 CA 证书）
/// </summary>
/// <param name="evssl">evssl_ctx</param>
/// <param name="mod">SSL_VERIFY_* </param>
/// <param name="vcb">SSL_verify_cb</param>
void evssl_verify(evssl_ctx *evssl, int32_t mod, SSL_verify_cb vcb);
/// <summary>
/// 设置 SSL 安全级别（OpenSSL 安全级别 0-5，默认 0）
/// </summary>
/// <param name="evssl">evssl_ctx</param>
/// <param name="level">安全级别：0 无限制，1 禁用 MD5/DES 等，2 禁用 RSA<2048 等</param>
void evssl_seclevel(evssl_ctx *evssl, int32_t level);
/// <summary>
/// 设置最低 TLS 协议版本（RFC 8996 建议 ≥ TLS 1.2）
/// </summary>
/// <param name="evssl">evssl_ctx</param>
/// <param name="version">
///        协议版本：
///            TLS1_VERSION(0x0301)
///            TLS1_1_VERSION(0x0302)
///            TLS1_2_VERSION(0x0303)
///            TLS1_3_VERSION(0x0304)
/// </param>
void evssl_min_proto(evssl_ctx *evssl, int32_t version);
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
/// 注册evssl_ctx；成功后所有权转移给证书池，失败时函数内自动释放 evssl，调用方不再持有
/// </summary>
/// <param name="name">名称, NULL 或 "" 表示无（注册失败）</param>
/// <param name="evssl">evssl_ctx</param>
/// <returns>ERR_OK 成功</returns>
int32_t evssl_register(const char *name, evssl_ctx *evssl);
/// <summary>
/// 根据名称获取evssl_ctx
/// </summary>
/// <param name="name">名称, NULL 或 "" 表示无（返回 NULL）</param>
/// <returns>evssl_ctx  NULL失败</returns>
evssl_ctx *evssl_qury(const char *name);
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
/// <returns>
///     ERR_OK:握手完成
///     1:需要读就绪（WANT_READ），重新注册读事件后重试
///     2:需要写就绪（WANT_WRITE），重新注册写事件后重试
///     ERR_FAILED:失败
///</returns>
int32_t evssl_tryacpt(SSL *ssl);
/// <summary>
/// 开始客户端握手
/// </summary>
/// <param name="ssl">SSL</param>
/// <returns>同 evssl_tryacpt</returns>
int32_t evssl_tryconn(SSL *ssl);
/// <summary>
/// 数据读取
/// </summary>
/// <param name="ssl">SSL</param>
/// <param name="buf">接收数据的buffer</param>
/// <param name="len">长度</param>
/// <param name="readed">读到的字节数；可为 0</param>
/// <returns>ERR_OK 表示"没出错"，含 WANT_READ / WANT_WRITE 两种要重试的情形，
///   此时 readed 可为 0。TLS1.3 数据期 SSL_read 也可能要求先写，调用方须在返回后自行
///   SSL_want_write() 探测并注册写事件，本函数不回传这个诉求</returns>
int32_t evssl_read(SSL *ssl, char *buf, size_t len, size_t *readed);
/// <summary>
/// 数据写入
/// </summary>
/// <param name="ssl">SSL</param>
/// <param name="buf">要写入的数据</param>
/// <param name="len">长度</param>
/// <param name="sended">写入的字节数；可为 0 或小于 len</param>
/// <returns>ERR_OK 表示"没出错"，含 WANT_READ / WANT_WRITE 两种要重试的情形。
///   未设 SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER，故重试**必须传同一个 buf 指针与同一 len**；
///   要写事件时调用方须自行 SSL_want_read() 探测，本函数不回传这个诉求</returns>
int32_t evssl_send(SSL *ssl, char *buf, size_t len, size_t *sended);
/// <summary>
/// shutdown
/// </summary>
/// <param name="ssl">SSL</param>
/// <param name="fd">socket句柄</param>
void evssl_shutdown(SSL *ssl, SOCKET fd);
/// <summary>
/// ssl版本，完成握手后调用
/// </summary>
/// <param name="ssl">SSL</param>
/// <returns>SSL3_VERSION TLS1_VERSION TLS1_1_VERSION TLS1_2_VERSION TLS1_3_VERSION</returns>
int32_t evssl_version(SSL *ssl);
/// <summary>
/// TLS1_3 keyupdate
/// </summary>
/// <param name="ssl">SSL</param>
/// <param name="updatetype">SSL_KEY_UPDATE_NOT_REQUESTED(单向更新) SSL_KEY_UPDATE_REQUESTED(双向更新)</param>
/// <returns>ERR_OK 仅表示已排程，KeyUpdate 消息要等下一次 SSL_write / SSL_do_handshake
///   才真正上线路；空闲连接上调它不会有任何报文发出</returns>
int32_t evssl_keyupdate(SSL *ssl, int32_t updatetype);

#endif//WITH_SSL
#endif//EVSSL_H_
