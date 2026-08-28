#ifndef YYJSON_HELPER_H_
#define YYJSON_HELPER_H_

#include "base/macro.h"
#include "serial/yyjson/yyjson.h"

/// <summary>
/// 从 JSON 对象读数值字段。取不到时 *val 保持原样不动，所以调用方可以先填好默认值再调。
/// 只挡 NaN / Inf；**范围由调用方自己验** —— 拿去 cast 成整型前必须确认值落在目标类型内，
/// 超范围的浮点转整型是未定义行为
/// </summary>
/// <param name="json">JSON 对象；非对象时按字段不存在处理</param>
/// <param name="name">字段名</param>
/// <param name="val">出参，必须非 NULL；仅在返回 ERR_OK 时被写入</param>
/// <returns>ERR_OK 已写入；字段不存在、非数字、或值为 NaN / Inf 时返回 ERR_FAILED，
/// 此时 *val 未被改动</returns>
int32_t json_get_number(yyjson_val *json, const char *name, double *val);
/// <summary>
/// 同 json_get_number，额外要求值落在 [min, max] 内。要把结果 cast 成整型时用这个，
/// min / max 传目标类型的取值区间——超范围的浮点转整型是未定义行为
/// </summary>
/// <param name="json">JSON 对象；非对象时按字段不存在处理</param>
/// <param name="name">字段名</param>
/// <param name="min">允许的下界</param>
/// <param name="max">允许的上界</param>
/// <param name="val">出参，必须非 NULL；仅在返回 ERR_OK 时被写入</param>
/// <returns>ERR_OK 已写入；json_get_number 失败、或值不在 [min, max] 内时返回
/// ERR_FAILED，此时 *val 未被改动</returns>
int32_t json_get_num_range(yyjson_val *json, const char *name, double min, double max, double *val);
/// <summary>
/// 从 JSON 对象读字符串字段并复制进定长缓冲。取不到时 str 保持原样不动，
/// 所以调用方可以先填好默认值再调，忽略返回值即可
/// </summary>
/// <param name="json">JSON 对象；非对象时按字段不存在处理</param>
/// <param name="name">字段名</param>
/// <param name="str">目标缓冲，必须非 NULL</param>
/// <param name="lens">缓冲大小，含结尾 '\0'</param>
/// <returns>ERR_OK 已写入；字段不存在、非字符串、或值长度不小于 lens 时返回
/// ERR_FAILED，此时 str 未被改动</returns>
int32_t json_get_string(yyjson_val *json, const char *name, char *str, size_t lens);

//yyjson 的默认分配器，已接到框架的 _malloc / _realloc / _free 上。
//yyjson 各 API 的 alc 形参传 NULL 即走它，无需显式指定；只有想换成 pool 分配器
//之类时才需要引用本变量。
extern const yyjson_alc g_yyjson_alc;

#endif//YYJSON_HELPER_H_
