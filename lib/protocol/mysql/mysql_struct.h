#ifndef MYSQL_STRUCT_H_
#define MYSQL_STRUCT_H_

#include "base/structs.h"
#include "utils/binary.h"
#include "containers/sarray.h"
#include "protocol/mysql/mysql_macro.h"

struct mpack_ctx;

// 客户端连接参数
typedef struct mysql_client_param {
    uint8_t charset;        // 字符集编码 ID
    uint16_t port;          // 服务器端口号
    uint32_t maxpack;       // 最大数据包大小（字节）
    uint32_t caps;          // 客户端能力标志位
    SOCKET fd;              // 连接套接字
    uint64_t skid;          // 套接字 ID（用于标识连接）
    struct evssl_ctx *evssl; // SSL 上下文，NULL 表示不启用 SSL
    char ip[IP_LENS];       // 服务器 IP 地址
    char user[64];          // 登录用户名
    char database[64];      // 默认数据库名
    char password[64];      // 登录密码
}mysql_client_param;

// 服务器握手参数（从握手包中解析）
typedef struct mysql_server_param {
    uint16_t status_flags;  // 服务器状态标志
    uint32_t caps;          // 服务器能力标志位
    char salt[20];          // 认证随机盐值
    char plugin[32];        // 认证插件名称
}mysql_server_param;

// MySQL 连接上下文
typedef struct mysql_ctx {
    int8_t id;              // 当前数据包序列号
    int8_t parse_status;    // 当前解析状态（结果集解析进度）
    uint8_t cur_cmd;        // 当前正在处理的命令类型
    int16_t error_code;     // 最近一次错误码
    int64_t last_id;        // 最近一次 INSERT 的自增 ID
    int64_t affected_rows;  // 最近一次操作影响的行数
    struct mpack_ctx *mpack; // 正在积累的数据包上下文（多包场景）
    struct task_ctx *task;   // 所属任务上下文
    mysql_server_param server; // 服务器握手参数
    mysql_client_param client; // 客户端连接参数
    char version[64];        // 服务器版本字符串
    char error_msg[256];     // 最近一次错误信息
}mysql_ctx;

// 参数绑定上下文（用于 COM_QUERY / COM_STMT_EXECUTE 的参数绑定）
typedef struct mysql_bind_ctx {
    int32_t count;          // 已绑定参数数量
    binary_ctx bitmap;      // NULL 位图缓冲区
    binary_ctx type;        // 参数类型缓冲区（不含名称，用于旧协议）
    binary_ctx type_name;   // 参数类型+名称缓冲区（用于 CLIENT_QUERY_ATTRIBUTES）
    binary_ctx value;       // 参数值缓冲区
}mysql_bind_ctx;

// MySQL 数据包上下文（解析后的响应包）
typedef struct mpack_ctx {
    int8_t sequence_id;     // 数据包序列号
    mpack_type pack_type;   // 数据包类型
    char *payload;          // 原始 payload 数据（由此结构体持有内存所有权）
    void *pack;             // 实际解析结果（mpack_ok / mpack_err / mysql_reader_ctx 等）
    void(*_free_mpack)(void *); // pack 字段的释放回调，NULL 表示直接 FREE
}mpack_ctx;

// OK 响应包数据
typedef struct mpack_ok {
    int16_t status_flags;   // 服务器状态标志
    int16_t warnings;       // 警告数量
    int64_t affected_rows;  // 影响的行数
    int64_t last_insert_id; // 最后插入的自增 ID
}mpack_ok;

// EOF 响应包数据
typedef struct mpack_eof {
    int16_t warnings;       // 警告数量
    int16_t status_flags;   // 服务器状态标志
}mpack_eof;

// 错误响应包数据
typedef struct mpack_err {
    int16_t error_code;     // 错误码
    buf_ctx error_msg;      // 错误信息
}mpack_err;

// 列字段描述信息（Column Definition）
typedef struct mpack_field {
    uint8_t decimals;       // 最大显示小数位数
    uint8_t type;           // 字段类型（enum_field_types）
    uint16_t flags;         // 列定义标志位
    int16_t character;      // 字符集 ID
    int32_t field_lens;     // 字段最大长度
    char schema[64];        // 所属 schema 名
    char table[64];         // 虚拟表名
    char org_table[64];     // 物理表名
    char name[64];          // 虚拟列名（别名）
    char org_name[64];      // 物理列名
}mpack_field;

// 一行数据中单个字段的值
typedef struct mpack_row {
    int32_t nil;            // 1 表示该字段值为 NULL
    buf_ctx val;            // 字段值数据（nil 为 0 且 val.data==NULL 时表示空字符串）
    char *payload;          // 行原始 payload 数据（仅第一个字段持有内存所有权）
}mpack_row;

// 查询结果集读取器（Resultset）
typedef struct mysql_reader_ctx {
    mpack_type pack_type;   // 结果集类型（MPACK_QUERY 或 MPACK_STMT_EXECUTE）
    int32_t field_count;    // 列数量
    int32_t index;          // 当前行游标
    mpack_field *fields;    // 列描述信息数组
    arr_ptr_ctx arr_rows;   // 行数据数组（每项为 mpack_row* ）
}mysql_reader_ctx;

// 预处理语句上下文
typedef struct mysql_stmt_ctx {
    uint16_t field_count;   // 结果集列数量
    uint16_t params_count;  // 参数数量
    int32_t index;          // 当前解析进度游标
    int32_t stmt_id;        // 服务器分配的语句 ID
    mpack_field *params;    // 参数字段描述数组
    mpack_field *fields;    // 结果集字段描述数组
    mysql_ctx *mysql;       // 所属 MySQL 连接上下文
}mysql_stmt_ctx;

#endif//MYSQL_STRUCT_H_
