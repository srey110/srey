#ifndef YYJSON_HELPER_H_
#define YYJSON_HELPER_H_

#include "base/macro.h"
#include "serial/yyjson/yyjson.h"

/// <summary>
/// 字段在不在。json_get_* 把"没配"和"配错了"合成同一个 ERR_FAILED，只想为后者告警时
/// 拿这个再判一次——可选字段缺席是正常的，不该跟着报错
/// </summary>
/// <param name="json">JSON 对象；非对象时恒返 0</param>
/// <param name="name">字段名</param>
/// <returns>存在返回非 0</returns>
int32_t json_has(yyjson_val *json, const char *name);
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
/// 同 json_get_number，但直接收已经取出的值（如 yyjson_obj_iter_get 按键序查到的），省一次按名查找
/// </summary>
/// <param name="jval">JSON 值；NULL 按字段不存在处理</param>
/// <param name="val">出参，必须非 NULL；仅在返回 ERR_OK 时被写入</param>
/// <returns>同 json_get_number</returns>
int32_t json_val_number(yyjson_val *jval, double *val);
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
/// 同 json_get_num_range，但直接收已经取出的值，口径同 json_val_number
/// </summary>
/// <param name="jval">JSON 值；NULL 按字段不存在处理</param>
/// <param name="min">允许的下界</param>
/// <param name="max">允许的上界</param>
/// <param name="val">出参，必须非 NULL；仅在返回 ERR_OK 时被写入</param>
/// <returns>同 json_get_num_range</returns>
int32_t json_val_num_range(yyjson_val *jval, double min, double max, double *val);
/// <summary>
/// 从 JSON 对象读字符串字段并复制进定长缓冲。取不到时 str 保持原样不动，
/// 所以调用方可以先填好默认值再调，忽略返回值即可
/// </summary>
/// <param name="json">JSON 对象；非对象时按字段不存在处理</param>
/// <param name="name">字段名</param>
/// <param name="str">目标缓冲，必须非 NULL</param>
/// <param name="lens">缓冲大小，含结尾 '\0'</param>
/// <returns>ERR_OK 已写入；字段不存在、非字符串、值含内嵌 NUL（目标是定长 C 串缓冲，
/// 收下就是静默改值）、或值长度不小于 lens 时返回 ERR_FAILED，此时 str 未被改动。
/// 长度按 JSON 记的真实字节数算，不是 strlen</returns>
int32_t json_get_string(yyjson_val *json, const char *name, char *str, size_t lens);
/// <summary>
/// 同 json_get_string，但直接收已经取出的值，口径同 json_val_number
/// </summary>
/// <param name="val">JSON 值；NULL 按字段不存在处理</param>
/// <param name="str">目标缓冲，必须非 NULL</param>
/// <param name="lens">缓冲大小，含结尾 '\0'</param>
/// <returns>同 json_get_string</returns>
int32_t json_val_string(yyjson_val *val, char *str, size_t lens);

//yyjson 的默认分配器，已接到框架的 _malloc / _realloc / _free 上。
//yyjson 各 API 的 alc 形参传 NULL 即走它，无需显式指定；只有想换成 pool 分配器
//之类时才需要引用本变量。
extern const yyjson_alc g_yyjson_alc;

#endif//YYJSON_HELPER_H_
