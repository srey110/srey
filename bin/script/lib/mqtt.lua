-- MQTT 模块封装：将 C 层 mqtt 模块与协议常量合并后返回。
-- 使用方：local mqtt = require("lib.mqtt")

local srey = require("lib.srey")
local cmqtt = require("mqtt")

cmqtt.VERSION = {
    V311 = 0x04,
    V50  = 0x05,
}
cmqtt.PROT = {
    RESERVED    = 0x00,
    CONNECT     = 0x01,
    CONNACK     = 0x02,
    PUBLISH     = 0x03,
    PUBACK      = 0x04,
    PUBREC      = 0x05,
    PUBREL      = 0x06,
    PUBCOMP     = 0x07,
    SUBSCRIBE   = 0x08,
    SUBACK      = 0x09,
    UNSUBSCRIBE = 0x0A,
    UNSUBACK    = 0x0B,
    PINGREQ     = 0x0C,
    PINGRESP    = 0x0D,
    DISCONNECT  = 0x0E,
    AUTH        = 0x0F,
}
cmqtt.PROP = {
    PAYLOAD_FORMAT           = 0x01,
    MSG_EXPIRY               = 0x02,
    CONTENT_TYPE             = 0x03,
    RESP_TOPIC               = 0x08,
    CORRELATION_DATA         = 0x09,
    SUBSCRIPTION_ID          = 0x0B,
    SESSION_EXPIRY           = 0x11,
    CLIENT_ID                = 0x12,
    SERVER_KEEPALIVE         = 0x13,
    AUTH_METHOD              = 0x15,
    AUTH_DATA                = 0x16,
    REQPROBLEM_INFO          = 0x17,
    WILLDELAY_INTERVAL       = 0x18,
    REQRESP_INFO             = 0x19,
    RESP_INFO                = 0x1A,
    SERVER_REFERENCE         = 0x1C,
    REASON_STR               = 0x1F,
    RECEIVE_MAXIMUM          = 0x21,
    TOPICALIAS_MAXIMUM       = 0x22,
    TOPIC_ALIAS              = 0x23,
    MAXIMUM_QOS              = 0x24,
    RETAIN_AVAILABLE         = 0x25,
    USER_PROPERTY            = 0x26,
    MAXIMUM_PACKETSIZE       = 0x27,
    WILDCARD_SUBSCRIPTION    = 0x28,
    SUBSCRIPTIONID_AVAILABLE = 0x29,
    SHARED_SUBSCRIPTION      = 0x2A,
}

---同步建立 MQTT 客户端连接：发起异步 try_connect → 等待 TCP（含 SSL）就绪
---@param version integer 协议版本 mqtt.VERSION.V311 / V50
---@param sslname SSL_NAME? SSL 上下文名；省略或 NONE 表示明文
---@param ip string 对端 IP
---@param port integer 对端端口
---@param netev NET_EV? 事件订阅掩码，默认 0
---@return integer fd socket fd；失败返回 INVALID_SOCK
---@return integer? skid 连接 skid；仅在 fd 有效时返回
function cmqtt.connect(version, sslname, ip, port, netev)
    local fd, skid = cmqtt.try_connect(version, sslname or SSL_NAME.NONE, ip, port, netev or 0)
    if INVALID_SOCK == fd then
        return INVALID_SOCK
    end
    -- wait_connect 内 `if nil ~= ssl` 把 false 视为 truthy 触发 SSL 等待，故显式 or nil 跳过
    local need_ssl = (sslname and SSL_NAME.NONE ~= sslname) or nil
    if not srey.wait_connect(fd, skid, need_ssl) then
        return INVALID_SOCK
    end
    return fd, skid
end

return cmqtt
