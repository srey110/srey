#ifndef PGSQL_STRUCT_H_
#define PGSQL_STRUCT_H_

#include "srey/spub.h"
#include "utils/binary.h"
#include "protocol/pgsql/pgsql_macro.h"

// pgsql 数据包上下文
typedef struct pgpack_ctx {
    pgpack_type type;               // 数据包类型（成功/错误/通知）
    void *pack;                     // 具体数据包内容（行读取器、错误信息或通知）
    void(*_free_pgpack)(void *);    // 释放 pack 的回调函数
    char complete[32];              // CommandComplete 命令完成标签，格式示例：INSERT oid rows / UPDATE rows 等
}pgpack_ctx;

// 异步通知消息（NotificationResponse）
typedef struct pgpack_notification {
    int32_t pid;            // 发送通知的后端进程 ID
    char *channel;          // 监听的频道名
    char *notification;     // 通知消息内容
    char *payload;          // 完整原始消息（用于内存管理）
}pgpack_notification;

// 行描述字段信息（RowDescription）
typedef struct pgpack_field {
    int16_t index;          // 列在所属表中的属性编号，无法识别时为 0
    int16_t lens;           // 数据类型大小（字节数），负值表示可变宽度类型
    pgpack_format format;   // 字段格式代码（文本或二进制）
    int32_t table_oid;      // 所属表的对象 ID
    int32_t type_oid;       // 数据类型的对象 ID
    int32_t type_modifier;  // 类型修饰符
    char name[64];          // 字段名称
}pgpack_field;

// 数据行中的单列值（DataRow）
typedef struct pgpack_row {
    int32_t lens;       // 列值的字节长度，0 表示空字符串，-1 表示 NULL
    char *val;          // 列值数据指针
    char *payload;      // 完整原始消息（首列持有，用于内存管理）
}pgpack_row;

// 查询结果读取器上下文
typedef struct pgsql_reader_ctx {
    int16_t field_count;        // 字段（列）数量
    pgpack_format format;       // 结果格式（文本或二进制）
    int32_t index;              // 当前读取行的游标位置
    pgpack_field *fields;       // 字段描述数组
    arr_ptr_ctx arr_rows;       // 数据行指针数组
}pgsql_reader_ctx;

// pgsql 连接上下文
typedef struct pgsql_ctx {
    int8_t readyforquery;       // 服务端就绪状态：'I' 空闲，'T' 事务块中，'E' 失败的事务块中
    uint16_t port;              // 服务端端口号
    int32_t pid;                // 后端进程 ID
    uint32_t key;               // 后端取消密钥
    SOCKET fd;                  // 套接字描述符
    uint64_t skid;              // 套接字唯一 ID
    struct task_ctx *task;      // 所属任务上下文
    struct evssl_ctx *evssl;    // SSL 上下文（不使用 SSL 时为 NULL）
    struct scram_ctx *scram;    // SCRAM 认证上下文（认证完成后释放）
    pgpack_ctx *pack;           // 当前正在累积的数据包
    char ip[IP_LENS];           // 服务端 IP 地址
    char user[64];              // 登录用户名
    char password[64];          // 登录密码
    char database[64];          // 目标数据库名
}pgsql_ctx;

// 预处理语句参数绑定上下文
typedef struct pgsql_bind_ctx {
    uint16_t nparam;        // 参数数量
    binary_ctx format;      // 各参数格式代码序列化缓冲区
    binary_ctx values;      // 各参数值序列化缓冲区
}pgsql_bind_ctx;

#endif//PGSQL_STRUCT_H_
