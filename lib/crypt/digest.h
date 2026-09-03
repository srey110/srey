#ifndef DIGEST_H_
#define DIGEST_H_

#include "crypt/md2.h"
#include "crypt/md4.h"
#include "crypt/md5.h"
#include "crypt/sha1.h"
#include "crypt/sha256.h"
#include "crypt/sha512.h"

#define DG_BLOCK_SIZE SHA512_BLOCK_SIZE
typedef void(*_init_cb)(void *);
typedef void(*_update_cb)(void *, const void *, size_t);
typedef void(*_final_cb)(void *, char *);

typedef enum digest_type {
    DG_MD2 = 0x01,
    DG_MD4,
    DG_MD5,
    DG_SHA1,
    DG_SHA256,
    DG_SHA512
}digest_type;
// 单个摘要算法的属性。digest.c 的算法表与 digest_ctx 共用这一份字段列表,
// 加一项只改这里,不会出现"表填了、ctx 忘拷"这种静默走样
typedef struct dg_attr {
    size_t block_lens;      // 当前摘要算法的输出长度（字节）
    size_t key_block;       // 压缩函数的输入分组长度 B（HMAC 的 ipad/opad 长度，不是输出长度）
    size_t eng_lens;        // 当前引擎 ctx 的实际字节数；eng_ctx 是联合体，按它拷贝而非整份
    _init_cb _init;         // 初始化回调
    _update_cb _update;     // 数据输入回调
    _final_cb _final;       // 结果输出回调
}dg_attr;
typedef struct digest_ctx {
    dg_attr attr;           // 算法属性，digest_init 从表里整份拷入
    union {
        md2_ctx md2;
        md4_ctx md4;
        md5_ctx md5;
        sha1_ctx sha1;
        sha256_ctx sha256;
        sha512_ctx sha512;
    }eng_ctx;               // 各算法上下文联合体
}digest_ctx;
/// <summary>
/// 初始化
/// </summary>
/// <param name="digest">digest_ctx</param>
/// <param name="dtype">摘要算法</param>
void digest_init(digest_ctx *digest, digest_type dtype);
/// <summary>
/// 清零整个 digest_ctx，含三个分发回调——调用后上下文即失效，要复用须重新 digest_init
/// </summary>
/// <param name="digest">digest_ctx</param>
void digest_free(digest_ctx *digest);
/// <summary>
/// 获取hash长度
/// </summary>
/// <param name="digest">digest_ctx</param>
/// <returns>长度</returns>
size_t digest_size(digest_ctx *digest);
/// <summary>
/// 填入数据
/// </summary>
/// <param name="digest">digest_ctx</param>
/// <param name="data">数据</param>
/// <param name="lens">数据长度</param>
void digest_update(digest_ctx *digest, const void *data, size_t lens);
/// <summary>
/// 计算hash
/// </summary>
/// <param name="digest">digest_ctx</param>
/// <param name="hash">输出缓冲，容量须 >= digest_init 所选算法的输出长度（即 digest_size 的返回值）；
/// 算法在编译期不确定时按 DG_BLOCK_SIZE 给，那是所有支持算法里最长的一个</param>
/// <returns>长度。返回后上下文已自动复位到初始状态，可直接开始下一条消息（无需再调 digest_reset）</returns>
size_t digest_final(digest_ctx *digest, char *hash);
/// <summary>
/// 重置,准备新一轮计算
/// </summary>
/// <param name="digest">digest_ctx</param>
void digest_reset(digest_ctx *digest);

#endif//DIGEST_H_
