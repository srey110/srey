#ifndef IDGEN_H_
#define IDGEN_H_

#include "base/base.h"

#define SERVICEID_MAX   0x7FFF //服务器id上限:id 占 createid 的高 16 位,最高位留 0 保证 id 恒为正数

/// <summary>
/// 设置服务器唯一id（用作 createid 的高16位），须在 createid 首次调用前于启动期设置一次
/// </summary>
/// <param name="id">服务器id，不得超过 SERVICEID_MAX</param>
/// <returns>ERR_OK 成功；id 超过 SERVICEID_MAX 时返回 ERR_FAILED</returns>
int32_t serviceid(uint16_t id);
/// <summary>
/// 获取全局唯一ID：高16位为服务器id(serviceid)，低48位为进程内计数。
/// 同一线程内递增，跨线程不保证先后（各线程按段领号），不能拿大小判断创建先后
/// </summary>
/// <returns>ID</returns>
uint64_t createid(void);
/// <summary>
/// 从 createid 生成的 ID 中解析出服务器 id（高 16 位）
/// </summary>
/// <param name="id">createid 返回的 ID</param>
/// <returns>服务器 id（与 serviceid 设置值一致，范围 0..0x7FFF）</returns>
static inline uint16_t parse_svid(uint64_t id) {
    return (uint16_t)(id >> 48);
}

#endif//IDGEN_H_
