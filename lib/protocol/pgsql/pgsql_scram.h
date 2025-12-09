#ifndef SCRAM_H_
#define SCRAM_H_

#include "utils/binary.h"
#include "crypt/hmac.h"
#include "crypt/base64.h"

#define SCRAM_NONCE_LEN	18

typedef struct scram_ctx {
    digest_type dtype;
    int32_t saltlen;//salt长度
    int32_t	iter;//轮数
    int32_t hslens;//hash数据长度
    int32_t svsignlens;//server_sign长度
    char *client_first_message_bare;//client_first_message n=..,r=...
    char *server_first_message;//server_first_message 全部
    char *client_final_message_without_proof;
    char *salt;//server_first_message s=的内容,base64解码后
    char *nonce;//server_first_message r=的内容
    char *server_sign;
    char client_nonce[B64EN_SIZE(SCRAM_NONCE_LEN) + 1];//客户端nonce base64编码后
    char saltedpwd[DG_BLOCK_SIZE];//Salted Password
}scram_ctx;

scram_ctx *scram_init(digest_type dtype);
void scram_free(scram_ctx *scram);
char *scram_client_first_message(scram_ctx *scram, const char *user);
int32_t scram_read_server_first_message(scram_ctx *scram, binary_ctx *breader);
char *scram_client_final_message(scram_ctx *scram, const char *pwd);
int32_t scram_server_final(scram_ctx *scram, binary_ctx *breader);

#endif//SCRAM_H_
