#ifndef PGSQL_STRUCT_H_
#define PGSQL_STRUCT_H_

#include "srey/spub.h"
#include "protocol/pgsql/pgsql_macro.h"

typedef struct pgpack_ctx {
    pgpack_type type;
    void *pack;
    void(*_free_pgpack)(void *);
    char complete[32];//CommandComplete 
}pgpack_ctx;
typedef struct pgpack_notification {
    int32_t pid;//后端进程的进程ID
    char *channel;//频道名
    char *notification;//消息
    char *payload;//完整消息
}pgpack_notification;
typedef struct pgpack_field {//RowDescription 行描述
    int16_t index;//如果该字段可以被识别为特定表格的列，则为该列的属性编号；否则为零
    int16_t lens;//数据类型大小 负值表示可变宽度类型
    pgpack_format format;//字段格式代码
    int32_t table_oid;//表的对象ID
    int32_t type_oid;//数据类型的对象ID  
    int32_t type_modifier;//类型修饰符
    char name[64];//字段名称
}pgpack_field;
typedef struct pgpack_row {//DataRow  数据行
    int32_t lens;//列值的长度  可以为零  -1表示空列值
    char *val;//列值
    char *payload;//完整消息
}pgpack_row;
typedef struct pgsql_reader_ctx {
    int16_t field_count;
    pgpack_format format;
    int32_t index;
    pgpack_field *fields;
    arr_ptr_ctx arr_rows;
}pgsql_reader_ctx;

typedef struct pgsql_ctx {
    int8_t scrammod;
    int8_t readyforquery;//'I': 空闲（不在事务块中） 'T': 事务块中  'E':失败的事务块中
    uint16_t port;
    int32_t pid;//后端的进程ID
    uint32_t key;//后端的秘钥
    SOCKET fd;
    uint64_t skid;
    struct task_ctx *task;
    struct evssl_ctx *evssl;
    struct scram_ctx *scram;
    pgpack_ctx *pack;
    char ip[IP_LENS];
    char user[64];
    char password[64];
    char database[64];
}pgsql_ctx;
typedef struct pgsql_bind_ctx {
    uint16_t nparam;
    buf_ctx *values;
    pgpack_format *format;
}pgsql_bind_ctx;

#endif//PGSQL_STRUCT_H_
