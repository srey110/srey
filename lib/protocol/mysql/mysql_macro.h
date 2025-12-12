#ifndef MYSQL_MACRO_H_
#define MYSQL_MACRO_H_

#define INT3_MAX                  0xFFFFFF
#define MYSQL_OK                  0x00
#define MYSQL_EOF                 0xfe
#define MYSQL_ERR                 0xff
#define MYSQL_QUIT                0x01
#define MYSQL_INIT_DB             0x02
#define MYSQL_QUERY               0x03
#define MYSQL_PING                0x0e
//STMT
#define MYSQL_PREPARE             0x16
#define MYSQL_EXECUTE             0x17
#define MYSQL_STMT_CLOSE          0x19
#define MYSQL_STMT_RESET          0x1a

#define MYSQL_LOCAL_INFILE         0xfb
#define MYSQL_HEAD_LENS            4
#define SERVER_MORE_RESULTS_EXISTS 8
#define MYSQL_AUTH_SWITCH          0xfe
#define MYSQL_CACHING_SHA2         0x01
#define MYSQL_CACHING_SHA2_FAST    0x03
#define MYSQL_CACHING_SHA2_FULL    0x04
#define CACHING_SHA2_PASSWORLD     "caching_sha2_password"
#define MYSQL_NATIVE_PASSWORLD     "mysql_native_password"

//Capabilities Flags
#define CLIENT_LONG_PASSWORD                  1 //旧密码插件
#define CLIENT_LONG_FLAG                      4 //Get all column flags
#define CLIENT_CONNECT_WITH_DB                8 //是否带有 dbname
#define CLIENT_IGNORE_SPACE                   256 //是否忽略 括号( 前面的空格
#define CLIENT_PROTOCOL_41                    512 //New 4.1 protocol. 
#define CLIENT_INTERACTIVE                    1024 //是否为交互式终端
#define CLIENT_SSL                            2048 //是否支持SSL
#define CLIENT_RESERVED2                      32768 //DEPRECATED: Old flag for 4.1 authentication \ CLIENT_SECURE_CONNECTION
#define CLIENT_MULTI_STATEMENTS               (1UL << 16) //是否支持multi-stmt.  COM_QUERY/COM_STMT_PREPARE中多条语句
#define CLIENT_MULTI_RESULTS                  (1UL << 17) //multi-results
#define CLIENT_PS_MULTI_RESULTS               (1UL << 18) //Multi-results and OUT parameters in PS-protocol.
#define CLIENT_PLUGIN_AUTH                    (1UL << 19) //是否支持密码插件 
#define CLIENT_CONNECT_ATTRS                  (1UL << 20) //client supports connection attributes
#define CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA (1UL << 21) //密码认证包能否大于255字节
#define CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS   (1UL << 22) //不关闭密码过期的连接
#define CLIENT_QUERY_ATTRIBUTES               (1UL << 27) //支持COM_QUERY/COM_STMT_EXECUTE中的可选参数

typedef enum mpack_type {
    MPACK_OK = 0x00,
    MPACK_ERR,
    MPACK_QUERY,
    MPACK_STMT_PREPARE,
    MPACK_STMT_EXECUTE
}mpack_type;
typedef enum mysql_field_types {
    MYSQL_TYPE_DECIMAL,
    MYSQL_TYPE_TINY,
    MYSQL_TYPE_SHORT,
    MYSQL_TYPE_LONG,
    MYSQL_TYPE_FLOAT,
    MYSQL_TYPE_DOUBLE,
    MYSQL_TYPE_NULL,
    MYSQL_TYPE_TIMESTAMP,
    MYSQL_TYPE_LONGLONG,
    MYSQL_TYPE_INT24,
    MYSQL_TYPE_DATE,
    MYSQL_TYPE_TIME,
    MYSQL_TYPE_DATETIME,
    MYSQL_TYPE_YEAR,
    MYSQL_TYPE_NEWDATE,
    MYSQL_TYPE_VARCHAR,
    MYSQL_TYPE_BIT,
    MYSQL_TYPE_TIMESTAMP2,
    MYSQL_TYPE_DATETIME2,
    MYSQL_TYPE_TIME2,
    MYSQL_TYPE_TYPED_ARRAY,
    MYSQL_TYPE_INVALID = 243,
    MYSQL_TYPE_BOOL = 244,
    MYSQL_TYPE_JSON = 245,
    MYSQL_TYPE_NEWDECIMAL = 246,
    MYSQL_TYPE_ENUM = 247,
    MYSQL_TYPE_SET = 248,
    MYSQL_TYPE_TINY_BLOB = 249,
    MYSQL_TYPE_MEDIUM_BLOB = 250,
    MYSQL_TYPE_LONG_BLOB = 251,
    MYSQL_TYPE_BLOB = 252,
    MYSQL_TYPE_VAR_STRING = 253,
    MYSQL_TYPE_STRING = 254,
    MYSQL_TYPE_GEOMETRY = 255
}mysql_field_types;

#endif//MYSQL_MACRO_H_
