#ifndef MYSQL_MACRO_H_
#define MYSQL_MACRO_H_

#define INT3_MAX                  0xFFFFFF          // 3字节无符号整数最大值
#define MYSQL_OK                  0x00              // MySQL 响应包类型：OK
#define MYSQL_EOF                 0xfe              // MySQL 响应包类型：EOF
#define MYSQL_ERR                 0xff              // MySQL 响应包类型：ERROR
#define MYSQL_QUIT                0x01              // 命令：断开连接
#define MYSQL_INIT_DB             0x02              // 命令：切换数据库
#define MYSQL_QUERY               0x03              // 命令：执行 SQL 查询
#define MYSQL_PING                0x0e              // 命令：PING 检测连接
//STMT
#define MYSQL_PREPARE             0x16              // 命令：预处理语句准备
#define MYSQL_EXECUTE             0x17              // 命令：预处理语句执行
#define MYSQL_STMT_CLOSE          0x19              // 命令：关闭预处理语句
#define MYSQL_STMT_RESET          0x1a              // 命令：重置预处理语句

#define MYSQL_LOCAL_INFILE         0xfb             // 本地文件加载标志
#define MYSQL_HEAD_LENS            4                // MySQL 数据包头部长度（字节）
#define SERVER_MORE_RESULTS_EXISTS 8                // 服务器状态标志：还有更多结果集
#define MYSQL_AUTH_SWITCH          0xfe             // 认证插件切换标志
#define MYSQL_CACHING_SHA2         0x01             // caching_sha2_password 认证状态标志
#define MYSQL_CACHING_SHA2_FAST    0x03             // caching_sha2 快速认证（密码已在缓存中）
#define MYSQL_CACHING_SHA2_FULL    0x04             // caching_sha2 完整认证（需要 RSA 加密）
#define CACHING_SHA2_PASSWORLD     "caching_sha2_password"  // caching_sha2_password 插件名
#define MYSQL_NATIVE_PASSWORLD     "mysql_native_password"  // mysql_native_password 插件名

//Capabilities Flags（客户端能力标志位）
#define CLIENT_LONG_PASSWORD                  1           // 使用新密码插件
#define CLIENT_LONG_FLAG                      4           // 获取全部列标志
#define CLIENT_CONNECT_WITH_DB                8           // 连接时携带数据库名
#define CLIENT_IGNORE_SPACE                   256         // 忽略括号前的空格
#define CLIENT_PROTOCOL_41                    512         // 使用 4.1 新协议
#define CLIENT_INTERACTIVE                    1024        // 交互式终端连接
#define CLIENT_SSL                            2048        // 支持 SSL 加密连接
#define CLIENT_RESERVED2                      32768       // 已废弃：4.1 安全连接标志
#define CLIENT_MULTI_STATEMENTS               (1UL << 16) // 支持 COM_QUERY/COM_STMT_PREPARE 中多条语句
#define CLIENT_MULTI_RESULTS                  (1UL << 17) // 支持多结果集
#define CLIENT_PS_MULTI_RESULTS               (1UL << 18) // 预处理语句支持多结果集及 OUT 参数
#define CLIENT_PLUGIN_AUTH                    (1UL << 19) // 支持可插拔认证插件
#define CLIENT_CONNECT_ATTRS                  (1UL << 20) // 支持连接属性
#define CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA (1UL << 21) // 认证数据长度可超过 255 字节
#define CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS   (1UL << 22) // 不因密码过期而断开连接
#define CLIENT_QUERY_ATTRIBUTES               (1UL << 27) // 支持 COM_QUERY/COM_STMT_EXECUTE 可选参数

// MySQL 数据包类型枚举
typedef enum mpack_type {
    MPACK_OK = 0x00,      // OK 响应包
    MPACK_ERR,            // 错误响应包
    MPACK_QUERY,          // 普通查询结果集
    MPACK_STMT_PREPARE,   // 预处理语句准备响应
    MPACK_STMT_EXECUTE    // 预处理语句执行结果集
}mpack_type;

// MySQL 字段类型枚举（对应 enum_field_types）
typedef enum mysql_field_types {
    MYSQL_TYPE_DECIMAL,       // DECIMAL 类型
    MYSQL_TYPE_TINY,          // TINYINT 类型
    MYSQL_TYPE_SHORT,         // SMALLINT 类型
    MYSQL_TYPE_LONG,          // INT 类型
    MYSQL_TYPE_FLOAT,         // FLOAT 类型
    MYSQL_TYPE_DOUBLE,        // DOUBLE 类型
    MYSQL_TYPE_NULL,          // NULL 类型
    MYSQL_TYPE_TIMESTAMP,     // TIMESTAMP 类型
    MYSQL_TYPE_LONGLONG,      // BIGINT 类型
    MYSQL_TYPE_INT24,         // MEDIUMINT 类型
    MYSQL_TYPE_DATE,          // DATE 类型
    MYSQL_TYPE_TIME,          // TIME 类型
    MYSQL_TYPE_DATETIME,      // DATETIME 类型
    MYSQL_TYPE_YEAR,          // YEAR 类型
    MYSQL_TYPE_NEWDATE,       // 内部新 DATE 类型
    MYSQL_TYPE_VARCHAR,       // VARCHAR 类型
    MYSQL_TYPE_BIT,           // BIT 类型
    MYSQL_TYPE_TIMESTAMP2,    // TIMESTAMP2 类型（精度扩展）
    MYSQL_TYPE_DATETIME2,     // DATETIME2 类型（精度扩展）
    MYSQL_TYPE_TIME2,         // TIME2 类型（精度扩展）
    MYSQL_TYPE_TYPED_ARRAY,   // 类型化数组（JSON 路径使用）
    MYSQL_TYPE_INVALID = 243, // 无效类型
    MYSQL_TYPE_BOOL = 244,    // BOOL 类型
    MYSQL_TYPE_JSON = 245,    // JSON 类型
    MYSQL_TYPE_NEWDECIMAL = 246,  // 精确 DECIMAL 类型
    MYSQL_TYPE_ENUM = 247,    // ENUM 类型
    MYSQL_TYPE_SET = 248,     // SET 类型
    MYSQL_TYPE_TINY_BLOB = 249,   // TINYBLOB 类型
    MYSQL_TYPE_MEDIUM_BLOB = 250, // MEDIUMBLOB 类型
    MYSQL_TYPE_LONG_BLOB = 251,   // LONGBLOB 类型
    MYSQL_TYPE_BLOB = 252,    // BLOB 类型
    MYSQL_TYPE_VAR_STRING = 253,  // VAR_STRING 类型
    MYSQL_TYPE_STRING = 254,  // STRING 类型
    MYSQL_TYPE_GEOMETRY = 255 // 空间几何类型
}mysql_field_types;

#endif//MYSQL_MACRO_H_
