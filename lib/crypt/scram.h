#ifndef SCRAM_H_
#define SCRAM_H_

#include "utils/binary.h"
#include "crypt/hmac.h"
#include "crypt/base64.h"

#define SCRAM_NONCE_LEN	18

typedef enum scram_status {
    SCRAM_INIT = 0x00,
    SCRAM_CLIENT_FIRST_MESSAGE,
    SCRAM_SERVER_FIRST_MESSAGE,
    SCRAM_CLIENT_FINAL_MESSAGE,
    SCRAM_SERVER_FINAL_MESSAGE
}scram_status;
typedef struct scram_ctx {
    scram_status status;
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

//SCRAM-SHA-1 SCRAM-SHA-256 
scram_ctx *scram_init(const char *method);
void scram_free(scram_ctx *scram);
//n,,n=,r=
char *scram_client_first_message(scram_ctx *scram, const char *user);
//r=,s=,i=
int32_t scram_server_first_message(scram_ctx *scram, char *msg, size_t mlens);
//c=biws,r=,p=
char *scram_client_final_message(scram_ctx *scram, const char *pwd);
//[e=] v=
int32_t scram_server_final_message(scram_ctx *scram, char *msg, size_t mlens);

#endif//SCRAM_H_
