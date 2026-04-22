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
/// 绑定参数
/// </summary>
/// <param name="bind">pgsql_bind_ctx</param>
/// <param name="value">值</param>
/// <param name="lens">值长度</param>
/// <param name="format">pgpack_format</param>
void pgsql_bind(pgsql_bind_ctx *bind, char *value, size_t lens, pgpack_format format);
void pgsql_bind_bool(pgsql_bind_ctx *bind, int8_t value);
void pgsql_bind_int16(pgsql_bind_ctx *bind, int16_t value);
void pgsql_bind_int32(pgsql_bind_ctx *bind, int32_t value);
void pgsql_bind_int64(pgsql_bind_ctx *bind, int64_t value);
void pgsql_bind_float(pgsql_bind_ctx *bind, float value);
void pgsql_bind_double(pgsql_bind_ctx *bind, double value);

#endif//PGSQL_BIND_H_
