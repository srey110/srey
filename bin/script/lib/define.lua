-- 全局常量定义：任务ID、协议类型、加密算法枚举及错误码。
-- 本文件在 main.c 启动前由 loader 加载，供所有 task 脚本共享。

-- 任务名常量表：值为字符串任务名（task.register/grab/call 据此寻址）；NONE(0) 为无效任务哨兵。
-- 句柄由 createid 运行期生成，业务按字符串名寻址；系统服务名须与 C / config.json 一致。
---@enum TASK_NAME
TASK_NAME = {
    NONE       = 0x00,
    HARBOR     = "harbor",
    DEBUG      = "debug",
}
---@enum TASK_TYPE
TASK_TYPE = {
    NORMAL = 0x00,
    MCO = 0x01,
    LUA = 0x02,
}
---框架保留的请求子类型(C 侧 subtype_t 为 uint16)。业务自定义的 reqtype 须避开这些值：
---REQ_DEBUG 被 srey.lua 的 _request_dispatch 拦截（仅未知命令才回落到 on_requested）；
---跨节点（srey.net_call / net_request）时被对端 harbor 按 spub.h 的 subtype_reserved 拒为 404。
---@enum REQUEST_TYPE
REQUEST_TYPE = {
    REQ_DEBUG             = 0x01, -- 调试命令
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
    AUTO   = 0x0000,-- 不设下限，用库支持的最低版本（OpenSSL 的默认状态）
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

    REDIS   = 0x20,
    MYSQL   = 0x21,
    PGSQL   = 0x22,
    MGDB    = 0x23,  -- MongoDB

    UDP_KCP = 0x40   -- KCP 可靠 UDP
}
-- 数据分片标志（对应 C 层 slice_type）
SLICE_TYPE = {
    START = 0x01,   -- 分片开始
    SLICE = 0x02,   -- 中间分片
    END   = 0x04,   -- 最后一片（完整消息）
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
    SHA512 = 0x06,
    XXH32  = 0x07,  -- 非密码学哈希，seed 固定为 0，输出大端 4 字节；hmac 不支持
    XXH64  = 0x08   -- 同 XXH32，输出大端 8 字节
}

INVALID_SOCK = -1 -- 无效 socket fd
ERR_OK     = 0 -- 操作成功
ERR_FAILED = -1 -- 操作失败

-- CLOSE 消息 erro 的取值（对应 C 层 close_type），连接是怎么断的
---@enum CLOSE_TYPE
CLOSE_TYPE = {
    ORDERLY   = 0,  -- 对端有序结束发送方向：裸 TCP 收到 FIN，SSL 收到 close_notify
    LOCAL     = 1,  -- 本地主动：close / task 拆除 / 发队列溢出 / 解析错误
    ABORT     = 2,  -- 异常中断：RST、读写错误、SSL 协议错
    TRUNCATED = 3,  -- TLS 没发 close_notify 就断了：字节可能已收全，也可能被截断，框架分不出
    NEVERCONN = 4   -- 连接/会话从未建立，本消息只为唤醒等待方，不触发 on_closed
}
