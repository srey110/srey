#ifndef MONGO_STRUCT_H_
#define MONGO_STRUCT_H_

#include "protocol/mongo/mongo_macro.h"

typedef struct mgopack_ctx {
    int8_t kind;     //消息 Section 类型：0 正文，1 文档序列
    uint32_t total;  //整个消息的总字节数（含消息头）
    int32_t reqid;   //当前消息的请求 ID
    int32_t respto;  //对应原始请求的 requestID（响应中使用）
    int32_t prot;    //协议操作码（mongo_prot）
    uint32_t klens;  //文档序列段总长度（kind == 1 时有效）
    uint32_t flags;  //消息标志位（mongo_flags）
    uint32_t dlens;  //doc 数据长度
    char *docid;     //文档序列标识符（kind == 1 时有效）
    char *doc;       //BSON 文档数据指针（指向 payload 内部）
    char *payload;   //完整消息原始数据缓冲区
}mgopack_ctx;

typedef struct mongo_session {
    int32_t timeoutmin; //会话超时时间（分钟）
    int32_t txnnumber;  //事务序号
    struct mongo_ctx *mongo; //所属连接上下文
    char *options;      //事务选项 BSON 数据（含 lsid/txnNumber/autocommit）
    uint64_t timeout;   //会话超时时间戳（毫秒）
    char uuid[UUID_LENS]; //会话 UUID
}mongo_session;
typedef struct mongo_ctx {
    uint16_t port;              //服务器端口
    int32_t reqid;              //当前请求 ID（自增）
    uint32_t flags;             //消息标志位（mongo_flags）
    SOCKET fd;                  //套接字文件描述符
    uint64_t skid;              //套接字唯一 ID
    mongo_session *session;     //当前会话（事务时非 NULL）
    struct task_ctx *task;      //所属任务上下文
    struct evssl_ctx *evssl;    //TLS 上下文，NULL 表示不加密
    struct scram_ctx *scram;    //SCRAM 认证上下文
    char *error;                //最近一次错误信息
    char ip[IP_LENS];           //服务器 IP 地址
    char db[64];                //当前数据库名
    char authdb[64];            //认证数据库名
    char collection[64];        //当前集合名
    char user[64];              //用户名
    char password[64];          //密码
}mongo_ctx;

#endif//MONGO_STRUCT_H_
