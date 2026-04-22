#ifndef SCRAM_H_
#define SCRAM_H_

#include "utils/binary.h"
#include "crypt/hmac.h"
#include "crypt/base64.h"

#define SCRAM_NONCE_LEN	18

typedef enum scram_status {
    SCRAM_INIT = 0x00,
    SCRAM_LOCAL_FIRST,
    SCRAM_REMOTE_FIRST,
    SCRAM_LOCAL_FINAL,
    SCRAM_REMOTE_FINAL
}scram_status;
typedef struct scram_ctx {
    int32_t client;//是否为发起端
    scram_status status;
    digest_type dtype;
    int32_t saltlen;//salt长度
    int32_t	iter;//轮数
    int32_t hslens;//hash数据长度
    char *local_first_message;//client n=,r=  server r=,s=,i=
    char *remote_first_message;//client r=,s=,i=  server n=,r=
    char *final_message_without_proof;//c=biws,r=
    char *salt;
    char *remote_nonce;
    char local_nonce[B64EN_SIZE(SCRAM_NONCE_LEN) + 1]; //base64
    char saltedpwd[DG_BLOCK_SIZE];//Salted Password
    char user[64];//user name
    char pwd[128];//password
}scram_ctx;
/*
client                                      server
SCRAM_INIT->SCRAM_LOCAL_FIRST               SCRAM_INIT->SCRAM_REMOTE_FIRST
scram_first_message                     ->  scram_parse_first_message                 n,,n=,r=

SCRAM_LOCAL_FIRST->SCRAM_REMOTE_FIRST       SCRAM_REMOTE_FIRST->SCRAM_LOCAL_FIRST
scram_parse_first_message               <-  scram_first_message                       r=,s=,i=

SCRAM_REMOTE_FIRST->SCRAM_LOCAL_FINAL       SCRAM_LOCAL_FIRST->SCRAM_REMOTE_FINAL
scram_final_message                     ->  scram_check_final_message                 c=biws,r=,p=

SCRAM_LOCAL_FINAL->SCRAM_REMOTE_FINAL       SCRAM_REMOTE_FINAL->SCRAM_LOCAL_FINAL
scram_check_final_message               <-  scram_final_message                       [e=] v=
*/
//SCRAM-SHA-1 SCRAM-SHA-256 SCRAM-SHA-512
scram_ctx *scram_init(const char *method, int32_t client);
void scram_free(scram_ctx *scram);
//client
void scram_set_user(scram_ctx *scram, const char *user);
//client server
void scram_set_pwd(scram_ctx *scram, const char *pwd);
//server
void scram_set_salt(scram_ctx *scram, char *salt, size_t lens);
//client server
void scram_set_iter(scram_ctx *scram, int32_t iter);
//server
const char *scram_get_user(scram_ctx *scram);
//client n,,n=,r=  server r=,s=,i=
char *scram_first_message(scram_ctx *scram);
//client r=,s=,i=  server n,,n=,r=
int32_t scram_parse_first_message(scram_ctx *scram, char *msg, size_t mlens);
//client c=biws,r=,p=  server [e=] v=
char *scram_final_message(scram_ctx *scram);
//client [e=] v=  server c=biws,r=,p=
int32_t scram_check_final_message(scram_ctx *scram, char *msg, size_t mlens);

#endif//SCRAM_H_
