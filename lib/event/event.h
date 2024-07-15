#ifndef EVENT_H_ 
#define EVENT_H_

#include "event/evpub.h"
#include "event/evssl.h"

/// <summary>
/// 网络初始化
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="nthreads">线程数</param>
void ev_init(ev_ctx *ctx, uint32_t nthreads);
/// <summary>
/// 网络释放
/// </summary>
/// <param name="ctx">ev_ctx</param>
void ev_free(ev_ctx *ctx);
/// <summary>
/// 监听
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="evssl">evssl_ctx, NULL 不使用</param>
/// <param name="ip">监听IP(0.0.0.0 - ::  127.0.0.1 - ::1)</param>
/// <param name="port">监听端口</param>
/// <param name="cbs">回调函数</param>
/// <param name="ud">用户数据</param>
/// <param name="id">监听ID</param>
/// <returns>ERR_OK 成功</returns>
int32_t ev_listen(ev_ctx *ctx, struct evssl_ctx *evssl, const char *ip, const uint16_t port,
    cbs_ctx *cbs, ud_cxt *ud, uint64_t *id);
/// <summary>
/// 链接
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="evssl">evssl_ctx, NULL 不使用</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="cbs">回调函数</param>
/// <param name="ud">用户数据</param>
/// <param name="skid">链接ID</param>
/// <returns>socket句柄</returns>
SOCKET ev_connect(ev_ctx *ctx, struct evssl_ctx *evssl, const char *ip, const uint16_t port,
    cbs_ctx *cbs, ud_cxt *ud, uint64_t *skid);
/// <summary>
/// 切换为SSL链接
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="client">1 作为客户端, 0 作为服务端</param>
/// <param name="evssl">evssl_ctx</param>
void ev_ssl(ev_ctx *ctx, SOCKET fd, uint64_t skid, int32_t client, struct evssl_ctx *evssl);
/// <summary>
/// UDP
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="cbs">回调函数</param>
/// <param name="ud">用户数据</param>
/// <param name="skid">链接ID</param>
/// <returns>socket句柄</returns>
SOCKET ev_udp(ev_ctx *ctx, const char *ip, const uint16_t port, cbs_ctx *cbs, ud_cxt *ud, uint64_t *skid);
/// <summary>
/// TCP发送数据
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="data">要发送的数据</param>
/// <param name="len">数据长度</param>
/// <param name="copy">1 拷贝数据, 0不拷贝数据</param>
void ev_send(ev_ctx *ctx, SOCKET fd, uint64_t skid, void *data, size_t len, int32_t copy);
/// <summary>
/// UDP发送数据
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="ip">IP</param>
/// <param name="port">端口</param>
/// <param name="data">要发送的数据</param>
/// <param name="len">数据长度</param>
/// <returns>ERR_OK 请求成功</returns>
int32_t ev_sendto(ev_ctx *ctx, SOCKET fd, uint64_t skid, const char *ip, const uint16_t port, void *data, size_t len);
/// <summary>
/// 关闭链接
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
void ev_close(ev_ctx *ctx, SOCKET fd, uint64_t skid);
/// <summary>
/// 取消监听
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="id">监听ID</param>
void ev_unlisten(ev_ctx *ctx, uint64_t id);
/// <summary>
/// 设置ud_cxt的数据包类型
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="pktype">数据包类型 pack_type</param>
void ev_ud_pktype(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint8_t pktype);
/// <summary>
/// 设置ud_cxt的状态
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="status">状态</param>
void ev_ud_status(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint8_t status);
/// <summary>
/// 设置ud_cxt的session
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="sess">session</param>
void ev_ud_sess(ev_ctx *ctx, SOCKET fd, uint64_t skid, uint64_t sess);
/// <summary>
/// 设置ud_cxt的任务名
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="name">任务名</param>
void ev_ud_name(ev_ctx *ctx, SOCKET fd, uint64_t skid, name_t name);
/// <summary>
/// 设置ud_cxt的extra
/// </summary>
/// <param name="ctx">ev_ctx</param>
/// <param name="fd">socket句柄</param>
/// <param name="skid">链接ID</param>
/// <param name="extra">extra</param>
void ev_ud_extra(ev_ctx *ctx, SOCKET fd, uint64_t skid, void *extra);

#endif//EVENT_H_
