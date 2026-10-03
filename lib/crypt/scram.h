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
    SCRAM_REMOTE_FINAL,   // 已发送/接收对端最终消息（认证完成）
    SCRAM_ERROR = 0xFF    // 解析失败终态：各入口 == 守卫均不匹配，拒绝重入与后续操作
}scram_status;
// 本端的通道绑定姿态。两端语义对称：都表示"本端具备多少绑定能力"，只是客户端据此决定发什么
// GS2 头，服务端据此决定收到什么 GS2 头该拒。由 scram_init（按机制名）与 scram_set_cbind 维护
typedef enum scram_cbind_mode {
    SCRAM_CB_NONE = 0,  // 无能力。客户端发 "n,,"；服务端不通告 -PLUS，"n"/"y" 都收
    SCRAM_CB_CAPABLE,   // 有材料但走的是非 PLUS 机制。客户端发 "y,,"；
                        // 服务端表示自己也通告了 -PLUS，此时收到 "y" 即降级攻击，必须拒（RFC 5802 §6）
    SCRAM_CB_PLUS       // PLUS 变体。客户端发 "p=<绑定类型>,,"；服务端只接受同一个头
}scram_cbind_mode;
typedef struct scram_ctx {
    scram_status status;                    // 当前握手状态
    digest_type dtype;                      // 摘要算法类型
    scram_cbind_mode cbind;                 // 通道绑定姿态
    int32_t client;                         // 1 为客户端，0 为服务端
    int32_t saltlen;                        // salt 长度（字节）
    int32_t iter;                           // 迭代轮数
    int32_t hslens;                         // 摘要输出长度（字节）
    int32_t cbind_len;                      // channel binding 数据长度（字节）
    char *local_first_message;              // 本端第一条消息（client: n=,r=  server: r=,s=,i=）
    char *remote_first_message;             // 对端第一条消息（client: r=,s=,i=  server: n=,r=）
    char *final_message_without_proof;      // 最终消息不含证明部分（c=...,r=）
    char *salt;                             // 服务端 salt（原始字节）
    char *remote_nonce;                     // 对端 nonce（base64 字符串）
    char *cbind_data;                       // channel binding 原始数据（tls-server-end-point 时为证书哈希）
    char *user;                             // 用户名（dup_zero 分配，scram_free 释放）
    char *pwd;                              // 密码（dup_zero 分配，scram_free 释放）
    char local_nonce[B64EN_SIZE(SCRAM_NONCE_LEN) + 1]; // 本端 nonce（base64 编码）
    char gs2_header[25];                    // 计算 c= 所用的 GS2 头（最长 "p=tls-server-end-point,," 24 字节）。
                                            // 客户端填本端选定的那个；服务端必须填对端实际发来的——
                                            // RFC 5802 §5 允许非 PLUS 端收到 "y,,"，若仍按 "n,," 重算则 c= 永不匹配
    char saltedpwd[DG_BLOCK_SIZE];          // SaltedPassword（PBKDF 输出）
}scram_ctx;
/* SCRAM 握手流程（左列为客户端，右列为服务端）：
client                                      server
SCRAM_INIT->SCRAM_LOCAL_FIRST               SCRAM_INIT->SCRAM_REMOTE_FIRST
scram_first_message                     ->  scram_parse_first_message                 [p=tls-server-end-point,,|n,,]n=,r=

SCRAM_LOCAL_FIRST->SCRAM_REMOTE_FIRST       SCRAM_REMOTE_FIRST->SCRAM_LOCAL_FIRST
scram_parse_first_message               <-  scram_first_message                       r=,s=,i=

SCRAM_REMOTE_FIRST->SCRAM_LOCAL_FINAL       SCRAM_LOCAL_FIRST->SCRAM_REMOTE_FINAL
scram_final_message                     ->  scram_check_final_message                 c=<cbind_b64>,r=,p=

SCRAM_LOCAL_FINAL->SCRAM_REMOTE_FINAL       SCRAM_REMOTE_FINAL->SCRAM_LOCAL_FINAL
scram_check_final_message               <-  scram_final_message                       [e=] v=
*/

/// <summary>
/// 创建并初始化 SCRAM 上下文
/// 支持 SCRAM-SHA-1、SCRAM-SHA-256、SCRAM-SHA-512 及其 -PLUS 变体（channel binding）
/// </summary>
/// <param name="method">方法名，如 "SCRAM-SHA-256" 或 "SCRAM-SHA-256-PLUS"</param>
/// <param name="client">1 为客户端，0 为服务端</param>
/// <returns>成功返回 scram_ctx 指针，不支持的方法返回 NULL</returns>
scram_ctx *scram_init(const char *method, int32_t client);
/// <summary>
/// 释放 SCRAM 上下文
/// </summary>
/// <param name="scram">scram_ctx</param>
void scram_free(scram_ctx *scram);
/// <summary>
/// 设置用户名（客户端握手前调用）
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <param name="user">用户名</param>
/// <param name="ulens">用户名长度（字节）</param>
/// <returns>ERR_OK 已生效；ERR_FAILED 未生效（user 为空，或内部含 0x00）——
/// 未生效时原值原样保留</returns>
int32_t scram_set_user(scram_ctx *scram, const char *user, size_t ulens);
/// <summary>
/// 设置密码（客户端和服务端均需调用）
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <param name="pwd">密码</param>
/// <param name="plens">密码长度（字节）；0 表示空密码，是允许的</param>
/// <returns>ERR_OK 已生效；ERR_FAILED 未生效（pwd 为 NULL，或内部含 0x00）——
/// 未生效时原值原样保留。从未设过密码则 scram_final_message 返回 NULL、
/// scram_check_final_message 返回 ERR_FAILED</returns>
int32_t scram_set_pwd(scram_ctx *scram, const char *pwd, size_t plens);
/// <summary>
/// 设置 salt（仅服务端调用）
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <param name="salt">salt 数据</param>
/// <param name="lens">salt 长度</param>
/// <returns>ERR_OK 已生效；ERR_FAILED 未生效（客户端角色调用，或 salt 为空）</returns>
int32_t scram_set_salt(scram_ctx *scram, char *salt, size_t lens);
/// <summary>
/// 设置迭代轮数（仅服务端调用）
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <param name="iter">迭代轮数，低于 SCRAM_MIN_ITER 提升到该下限，高于 SCRAM_MAX_ITER 夹到该上限
///     （与客户端解析服务端 i= 时的上限同值，故夹过的值对端照样接受）</param>
/// <returns>ERR_OK 已生效；ERR_FAILED 未生效（客户端角色调用）</returns>
int32_t scram_set_iter(scram_ctx *scram, int32_t iter);
/// <summary>
/// 交给 scram 本端拿到的 channel binding 材料（tls-server-end-point 即服务端证书 SHA-256 哈希），
/// 由它按机制决定怎么用：
///   PLUS 变体   —— 存下来用于计算 / 校验 c=。两端都必须调，漏调时 scram_final_message
///                  返回 NULL、scram_check_final_message 返回 ERR_FAILED
///   非 PLUS 变体 —— 记下"本端有材料"（SCRAM_CB_CAPABLE）：客户端把 GS2 头 "n,," 改成 "y,,"，
///                  服务端收到 "y" 而自己其实支持 PLUS 即判定降级、拒绝握手
/// 挡不住"中间人已持有本端信任的证书"——本机制只让通道绑定的失效可检测，不是不可绕过
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <param name="data">channel binding 原始数据</param>
/// <param name="lens">数据长度</param>
/// <returns>ERR_OK 已生效；ERR_FAILED 未生效（data 为空，或非 PLUS 变体下状态已过 SCRAM_INIT）</returns>
int32_t scram_set_cbind(scram_ctx *scram, const char *data, size_t lens);
/// <summary>
/// 获取客户端用户名（仅服务端调用）
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <returns>用户名字符串</returns>
const char *scram_get_user(scram_ctx *scram);
/// <summary>
/// 生成第一条消息（客户端: [GS2]n=,r=  服务端: r=,s=,i=）
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <returns>消息字符串（调用方负责释放）</returns>
char *scram_first_message(scram_ctx *scram);
/// <summary>
/// 解析对端第一条消息（客户端解析 r=,s=,i=  服务端解析 [GS2]n=,r=）
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <param name="msg">消息数据</param>
/// <param name="mlens">消息长度</param>
/// <returns>ERR_OK 成功，ERR_FAILED 失败</returns>
int32_t scram_parse_first_message(scram_ctx *scram, char *msg, size_t mlens);
/// <summary>
/// 生成最终消息（客户端: c=<cbind_b64>,r=,p=  服务端: [e=] v=）
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <returns>消息字符串；状态不符、未设置密码、PLUS 变体未设置 channel binding 数据均返回 NULL。
/// 客户端侧返回值含 ClientProof（配上线上抓到的 AuthMessage 即可还原 ClientKey），
/// 调用方须用 SECURE_FREE(msg, strlen(msg) + 1) 释放，不可裸 FREE</returns>
char *scram_final_message(scram_ctx *scram);
/// <summary>
/// 验证对端最终消息（客户端验证 [e=] v=  服务端验证 c=<cbind_b64>,r=,p=）
/// </summary>
/// <param name="scram">scram_ctx</param>
/// <param name="msg">消息数据</param>
/// <param name="mlens">消息长度</param>
/// <returns>ERR_OK 验证通过，ERR_FAILED 验证失败</returns>
int32_t scram_check_final_message(scram_ctx *scram, char *msg, size_t mlens);

#endif//SCRAM_H_
