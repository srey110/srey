#ifndef SCRAM_H_
#define SCRAM_H_

#include "utils/binary.h"
#include "crypt/hmac.h"
#include "crypt/base64.h"

#define SCRAM_NONCE_LEN 18 // 随机 nonce 原始字节长度

typedef enum scram_status {
    SCRAM_INIT = 0x00,    // 初始状态
    SCRAM_LOCAL_FIRST,    // 已发送/接收本端第一条消息
    SCRAM_REMOTE_FIRST,   // 已发送/接收对端第一条消息
    SCRAM_LOCAL_FINAL,    // 已发送/接收本端最终消息
    SCRAM_REMOTE_FINAL    // 已发送/接收对端最终消息（认证完成）
}scram_status;
typedef struct scram_ctx {
    int32_t client;                         // 1 为客户端，0 为服务端
    scram_status status;                    // 当前握手状态
    digest_type dtype;                      // 摘要算法类型
    int32_t saltlen;                        // salt 长度（字节）
    int32_t iter;                           // 迭代轮数
    int32_t hslens;                         // 摘要输出长度（字节）
    char *local_first_message;              // 本端第一条消息（client: n=,r=  server: r=,s=,i=）
    char *remote_first_message;             // 对端第一条消息（client: r=,s=,i=  server: n=,r=）
    char *final_message_without_proof;      // 最终消息不含证明部分（c=biws,r=）
    char *salt;                             // 服务端 salt（原始字节）
    char *remote_nonce;                     // 对端 nonce（base64 字符串）
    char local_nonce[B64EN_SIZE(SCRAM_NONCE_LEN) + 1]; // 本端 nonce（base64 编码）
    char saltedpwd[DG_BLOCK_SIZE];          // SaltedPassword（PBKDF 输出）
    char user[64];                          // 用户名
    char pwd[128];                          // 密码
}scram_ctx;
/* SCRAM 握手流程（左列为客户端，右列为服务端）：
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
/// <summary>
/// 创建并初始化 SCRAM 上下文，支持 SCRAM-SHA-1、SCRAM-SHA-256、SCRAM-SHA-512
/// </summary>
/// <param name="method">方法名，如 "SCRAM-SHA-256"</param>
/// <param name="client">1 为客户端，0 为服务端</param>
/// <returns>成功返回 scram_ctx 指针，不支持的方法返回 NULL</returns>
scram_ctx *scram_init(const char *method, int32_t client);
/// <summary>
/// 释放 SCRAM 上下文
/// </summary>
/// <param name="scram">scram_ctx</param>
void scram_free(scram_ctx *scram);
/// <summary>
/// 设置用户名（仅客户端调用）
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <param name="user">用户名</param>
void scram_set_user(scram_ctx *scram, const char *user);
/// <summary>
/// 设置密码（客户端和服务端均需调用）
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <param name="pwd">密码</param>
void scram_set_pwd(scram_ctx *scram, const char *pwd);
/// <summary>
/// 设置 salt（仅服务端调用）
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <param name="salt">salt 数据</param>
/// <param name="lens">salt 长度</param>
void scram_set_salt(scram_ctx *scram, char *salt, size_t lens);
/// <summary>
/// 设置迭代轮数（仅服务端调用）
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <param name="iter">迭代轮数</param>
void scram_set_iter(scram_ctx *scram, int32_t iter);
/// <summary>
/// 获取客户端用户名（仅服务端调用）
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <returns>用户名字符串</returns>
const char *scram_get_user(scram_ctx *scram);
/// <summary>
/// 生成并返回第一条消息（客户端: n,,n=,r=  服务端: r=,s=,i=）
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <returns>消息字符串（调用方负责释放）</returns>
char *scram_first_message(scram_ctx *scram);
/// <summary>
/// 解析对端第一条消息（客户端解析 r=,s=,i=  服务端解析 n,,n=,r=）
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <param name="msg">消息数据</param>
/// <param name="mlens">消息长度</param>
/// <returns>ERR_OK 成功，ERR_FAILED 失败</returns>
int32_t scram_parse_first_message(scram_ctx *scram, char *msg, size_t mlens);
/// <summary>
/// 生成并返回最终消息（客户端: c=biws,r=,p=  服务端: [e=] v=）
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <returns>消息字符串（调用方负责释放）</returns>
char *scram_final_message(scram_ctx *scram);
/// <summary>
/// 验证对端最终消息（客户端验证 [e=] v=  服务端验证 c=biws,r=,p=）
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <param name="msg">消息数据</param>
/// <param name="mlens">消息长度</param>
/// <returns>ERR_OK 验证通过，ERR_FAILED 验证失败</returns>
int32_t scram_check_final_message(scram_ctx *scram, char *msg, size_t mlens);

#endif//SCRAM_H_
