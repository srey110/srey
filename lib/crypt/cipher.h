#ifndef CIPHER_H_
#define CIPHER_H_

#include "crypt/aes.h"
#include "crypt/des.h"
#include "crypt/padding.h"

#define CIPHER_BLOCK_SIZE AES_BLOCK_SIZE
typedef char *(*_cipher_cb)(void *, const void *);
//加解密类型
typedef enum engine_type {
    DES = 0x01,
    DES3,
    AES
}engine_type;
//加解密模式
typedef enum cipher_model {
    ECB = 0x01,
    CBC,
    CFB,
    OFB,
    CTR
}cipher_model;
typedef struct cipher_ctx {
    cipher_model model;                 // 加解密模式
    padding_model padding;              // 填充模式
    int32_t encrypt;                    // 1 加密，0 解密
    size_t block_lens;                  // 分组长度（字节）
    _cipher_cb _cipher;                 // 当前引擎的加解密回调
    uint8_t iv[CIPHER_BLOCK_SIZE];      // 初始 IV
    uint8_t cur_iv[CIPHER_BLOCK_SIZE];  // 当前 IV（每次分组后更新）
    uint8_t pd_data[CIPHER_BLOCK_SIZE]; // 填充数据缓冲区
    uint8_t xor_data[CIPHER_BLOCK_SIZE];// 异或中间结果缓冲区
    union {
        aes_ctx aes;
        des_ctx des;
    }eng_ctx;                           // 引擎上下文联合体
}cipher_ctx;

/// <summary>
/// 加解密初始化
/// </summary>
/// <param name="cipher">cipher_ctx</param>
/// <param name="engine">加解密类型</param>
/// <param name="model">加解密模式</param>
/// <param name="key">密码</param>
/// <param name="klens">密码长度</param>
/// <param name="keybits">engine为AES,值 128 192 256</param>
/// <param name="encrypt">1 加密 0 解密</param>
void cipher_init(cipher_ctx *cipher, engine_type engine, cipher_model model,
    const char *key, size_t klens, int32_t keybits, int32_t encrypt);
/// <summary>
/// 清零整个 cipher_ctx，连引擎回调一起抹掉——调用后上下文即失效，要复用须重新 cipher_init
/// </summary>
/// <param name="cipher">cipher_ctx</param>
void cipher_free(cipher_ctx *cipher);
/// <summary>
/// 获取分组长度
/// </summary>
/// <param name="cipher">cipher_ctx</param>
/// <returns>长度</returns>
size_t cipher_size(cipher_ctx *cipher);
/// <summary>
/// 设置填充模式.ECB CBC需要  CFB OFB CTR可选。
/// 注意 ZeroPadding 不对称：加密侧按块补零，解密侧不剥离——尾部真实的 0 与填充的 0 无从区分，
/// cipher_dofinal 于是返回含补零的完整长度。要能还原原始长度请用 PKCS57 / ANSIX923 / ISO10126
/// </summary>
/// <param name="cipher">cipher_ctx</param>
/// <param name="padding">填充模式</param>
void cipher_padding(cipher_ctx *cipher, padding_model padding);
/// <summary>
/// 设置IV向量. CBC CFB OFB CTR需要
/// </summary>
/// <param name="cipher">cipher_ctx</param>
/// <param name="iv">IV</param>
/// <param name="ilens">IV长度,小于分组长度会自动补 0 填充;
///     大于分组长度只取前 cipher_size 字节，多出来的部分静默丢弃</param>
void cipher_iv(cipher_ctx *cipher, const char *iv, size_t ilens);
/// <summary>
/// 重置,准备新一轮加解密
/// </summary>
/// <param name="cipher">cipher_ctx</param>
void cipher_reset(cipher_ctx *cipher);
/// <summary>
/// 加解密分组数据
/// </summary>
/// <param name="cipher">cipher_ctx</param>
/// <param name="data">要加解密的数据</param>
/// <param name="lens">数据长度,小于等于分组长度</param>
/// <param name="size">加解密后的长度</param>
/// <returns>指向 cipher_ctx 内部缓冲的结果,不是 malloc 来的,调用方不得释放,有效期到下次
///   cipher_block / cipher_reset 为止。下列输入返 NULL：lens 超过分组长度、ECB/CBC 下
///   给了不足一个分组、填充失败。反之——整分组输入且已选填充模式——不会为 NULL</returns>
void *cipher_block(cipher_ctx *cipher, const void *data, size_t lens, size_t *size);
/// <summary>
/// 加解密一段数据
/// </summary>
/// <param name="cipher">cipher_ctx</param>
/// <param name="data">要加解密的数据</param>
/// <param name="lens">数据长度</param>
/// <param name="output">加解密后的数据,预估长度:lens + 分组长度</param>
/// <param name="outlens">输出:加解密后的长度;失败时置 0</param>
/// <returns>ERR_OK 成功;ERR_FAILED 失败（NoPadding+ECB/CBC 输入非对齐、解密时长度不足一个
/// 分组、或填充校验不通过），output 已清零。失败不能靠"长度为 0"判断——加密空明文再解密
/// 回来本就是 0 字节的合法结果,两者必须分开</returns>
/// <remarks>
/// CTR/CFB/OFB 模式：函数内部会 cipher_reset 将 cur_iv 重置为初始 iv；
/// 因此跨多条独立消息复用同一 cipher_ctx 时，调用方必须在每条消息前
/// 通过 cipher_iv 设置新 IV，否则流密钥相同会导致密文可被异或推明文。
/// </remarks>
int32_t cipher_dofinal(cipher_ctx *cipher, const void *data, size_t lens, char *output, size_t *outlens);

#endif//CIPHER_H_
