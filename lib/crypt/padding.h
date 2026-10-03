#ifndef PADDING_H_
#define PADDING_H_

#include "base/base.h"

typedef enum padding_model {
    NoPadding = 0x00, // 不填充
    ZeroPadding,      // 零字节填充
    PKCS57,           // PKCS#7 填充（填充字节值等于填充长度）
    ISO10126,         // ISO 10126 填充（随机字节 + 末尾填充长度）
    ANSIX923          // ANSI X.923 填充（零字节 + 末尾填充长度）
}padding_model;

/// <summary>
/// 数据填充
/// </summary>
/// <param name="padding">填充模式；NoPadding 只拷贝 data，填充区保持调用方原样不写</param>
/// <param name="data">需要填充的数据；仅 dlens 为 0 时允许传 NULL</param>
/// <param name="dlens">数据长度；必须 &lt;= reqlens</param>
/// <param name="output">输出填充后的数据；仅 ERR_OK 时有效</param>
/// <param name="reqlens">要求的数据长度</param>
/// <returns>ERR_OK 成功；ERR_FAILED：dlens 超出 reqlens，dlens 非 0 却传了 NULL data，
/// 或 ISO10126 取随机字节失败。失败返回时 output 内容一律无效，调用方不能当它有内容；
/// 前两种一个字节都没写，取随机失败那种可能已写入前 dlens 字节</returns>
int32_t _padding_data(padding_model padding, const void *data, size_t dlens, uint8_t *output, size_t reqlens);
/// <summary>
/// 密码填充
/// </summary>
/// <param name="key">密码</param>
/// <param name="klens">密码长度</param>
/// <param name="pdkey">储存填充的密码</param>
/// <param name="reqlens">要求的密码长度</param>
/// <returns>klens 小于 reqlens 时返回 pdkey（前 klens 字节为 key，其余补 0）；
/// klens 不小于 reqlens 时原样返回 key，只有前 reqlens 字节会被用到，多出来的部分静默丢弃</returns>
uint8_t *_padding_key(const char *key, size_t klens, uint8_t *pdkey, size_t reqlens);

#endif//PADDING_H_
