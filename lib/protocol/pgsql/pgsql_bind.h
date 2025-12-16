#ifndef PGSQL_BIND_H_
#define PGSQL_BIND_H_

#include "protocol/pgsql/pgsql_struct.h"

/// <summary>
/// pgsql_bind_ctx初始化
/// </summary>
/// <param name="bind">pgsql_bind_ctx</param>
/// <param name="nparam">参数数量</param>
void pgsql_bind_init(pgsql_bind_ctx *bind, uint16_t nparam);
/// <summary>
/// pgsql_bind_ctx释放
/// </summary>
/// <param name="bind">pgsql_bind_ctx</param>
void pgsql_bind_free(pgsql_bind_ctx *bind);
/// <summary>
/// pgsql_bind_ctx清空
/// </summary>
/// <param name="bind">pgsql_bind_ctx</param>
void pgsql_bind_clear(pgsql_bind_ctx *bind);
/// <summary>
/// 绑定参数(不会复制)
/// </summary>
/// <param name="bind">pgsql_bind_ctx</param>
/// <param name="index">序号[0 - nparam)</param>
/// <param name="val">值</param>
/// <param name="lens">值长度</param>
/// <param name="format">pgpack_format</param>
void pgsql_bind(pgsql_bind_ctx *bind, uint16_t index, char *val, size_t lens, pgpack_format format);

#endif//PGSQL_BIND_H_
