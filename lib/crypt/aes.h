#ifndef AES_H_
#define AES_H_

#include "base/macro.h"

#define AES_BLOCK_SIZE 16
typedef struct aes_ctx {
    int32_t encrypt;
    int32_t nrounds;
    uint8_t output[AES_BLOCK_SIZE];
    uint32_t schedule[256 / 8 + 28];
}aes_ctx;
/// <summary>
/// aes ��ʼ��
/// </summary>
/// <param name="aes">aes_ctx</param>
/// <param name="key">����</param>
/// <param name="klens">���볤��, ����16 24 32 �����0</param>
/// <param name="keybits">128 192 256</param>
/// <param name="encrypt">1 ����, 0 ����</param>
void aes_init(aes_ctx *aes, const char *key, size_t klens, int32_t keybits, int32_t encrypt);
/// <summary>
/// aes�ӽ���
/// </summary>
/// <param name="aes">aes_ctx</param>
/// <param name="data">���ӽ�������,����:AES_BLOCK_SIZE</param>
/// <returns>�ӽ��ܺ������,����:AES_BLOCK_SIZE</returns>
char *aes_crypt(aes_ctx *aes, const void *data);

#endif//AES_H_
