-- 全局常量定义：任务ID、协议类型、加密算法枚举及错误码。
-- 本文件在 main.c 启动前由 loader 加载，供所有 task 脚本共享。

-- 任务名常量表：值为字符串任务名（task.register/grab/call 据此寻址）；NONE(0) 为无效任务哨兵。
-- 句柄由 createid 运行期生成，业务按字符串名寻址；系统服务名须与 C / config.json 一致。
---@enum TASK_NAME
TASK_NAME = {
    NONE       = 0x00,
    HARBOR     = "harbor",
    DEBUG      = "debug",
    DATACENTER = "datacenter",  -- 全局 KV + wait/唤醒 task service(C 实现,loader_init 后自动注册)
    SUBCENTER  = "subcenter",   -- 订阅中心 task service(C 实现,loader_init 后自动注册)
}
---@enum TASK_TYPE
TASK_TYPE = {
    NORMAL = 0x00,
    MCO = 0x01,
    LUA = 0x02,
}
---@enum REQUEST_TYPE
REQUEST_TYPE = {
    REQ_DEBUG      = 0x01, -- 调试命令（lib/services/debug_console.c → 目标 task）
    REQ_DC         = 0x10, -- DataCenter:统一入口,子命令由 payload 首字节 u8 op 标识(见 lib/services/datacenter.c dc_op)
    REQ_SC         = 0x20, -- subcenter 订阅中心:统一入口,子命令由 payload 首字节 u8 op 标识(见 lib/services/subcenter.c sc_op)
    REQ_SC_DELIVER = 0x21, -- subcenter → 订阅者推送:wire=|kind|publisher|mlen|meta|glen|group|tlen|topic|plen|payload|
}
-- MySQL 响应包类型，与 C 层 mysql_pack_type 枚举一一对应。
---@enum MYSQL_PACK_TYPE
MYSQL_PACK_TYPE =  {
    MPACK_OK = 0x00,     -- 命令执行成功（无结果集）
    MPACK_ERR = 0x01,    -- 服务端返回错误
    MPACK_QUERY = 0x02,  -- 查询结果集
    MPACK_STMT_PREPARE = 0x03,  -- 预处理语句准备响应
    MPACK_STMT_EXECUTE = 0x04   -- 预处理语句执行响应
}
-- SSL 上下文名称；与 C 层 ssl_name 枚举对应。
-- NONE 表示不启用 TLS，SERVER/CLIENT 分别对应服务端和客户端证书上下文。
---@enum SSL_NAME
SSL_NAME = {
    NONE =   "",
    SERVER = "server",
    CLIENT = "client"
}
-- TLS 协议版本；用于 core.ssl_min_proto() 设置最低允许版本
---@enum TLS_VERSION
TLS_VERSION = {
    TLS1_0 = 0x0301,
    TLS1_1 = 0x0302,
    TLS1_2 = 0x0303,
    TLS1_3 = 0x0304
}
-- SSL 证书文件格式
---@enum SSLFILE_TYPE
SSLFILE_TYPE = {
    PEM  = 0x01,   -- PEM 文本格式
    ASN1 = 0x02    -- ASN.1/DER 二进制格式
}
-- 网络事件标志（位掩码，可组合使用）
---@enum NET_EV
NET_EV = {
    NONE    = 0x00,
    ACCEPT  = 0x01,   -- 监听接受新连接
    AUTHSSL = 0x02,   -- 自动触发 SSL 握手
    SEND    = 0x04    -- 关注发送完成事件
}
-- Socket 协议类型，标识该 socket 所使用的应用层协议；与 C 层 pack_type 枚举对应。
---@enum PACK_TYPE
PACK_TYPE = {
    NONE    = 0x00,
    DNS     = 0x01,
    HTTP    = 0x02,
    WEBSOCK = 0x03,
    MQTT    = 0x04,
    SMTP    = 0x05,
    CUSTZ_FIXED =  0x06,  -- 自定义：固定长度包头
    CUSTZ_FLAG = 0x07,    -- 自定义：标志分隔
    CUSTZ_VAR = 0x08,     -- 自定义：变长包

    REDIS   = 0x50,
    MYSQL   = 0x51,
    PGSQL   = 0x52,
    MGDB    = 0x53   -- MongoDB
}
-- 对称加密算法类型
---@enum CIPHER_TYPE
CIPHER_TYPE = {
    DES  = 0x01,
    DES3 = 0x02,
    AES  = 0x03
}
-- 分组加密工作模式
---@enum CIPHER_MODEL
CIPHER_MODEL = {
    ECB = 0x01,
    CBC = 0x02,
    CFB = 0x03,
    OFB = 0x04,
    CTR = 0x05
}
-- 填充方案；注意 PKCS#7 在此实现中枚举名为 PKCS57
---@enum PADDING_MODEL
PADDING_MODEL = {
    NoPadding   = 0x00,
    ZeroPadding = 0x01,
    PKCS57      = 0x02,   -- PKCS#7 填充
    ISO10126    = 0x03,
    ANSIX923    = 0x04
}
-- 摘要算法类型
---@enum DIGEST_TYPE
DIGEST_TYPE = {
    MD2    = 0x01,
    MD4    = 0x02,
    MD5    = 0x03,
    SHA1   = 0x04,
    SHA256 = 0x05,
    SHA512 = 0x06
}

ERR_OK     = 0    -- 操作成功
ERR_FAILED = -1   -- 操作失败
INVALID_SOCK = -1 -- 无效 socket fd
