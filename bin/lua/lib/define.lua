--任务名
TASK_NAME = {
    NONE = 0x00,--保留
    HARBOR = 0x01,
    TEST1 = 10001,
    TEST2 = 10002,
    TEST3 = 10003,
    TEST4 = 10004,
    TEST_WBSK = 10005,
    TEST_HTTP = 10006
}
--SSL名
SSL_NAME = {
    NONE = 0x00,
    SERVER = 0x01,
    CLIENT = 0x02
}
--包类型 标识该socket类型 与c pack_type对应
PACK_TYPE = {
    NONE = 0x00,
    RPC = 0x01,
    HTTP = 0x02,
    WEBSOCK = 0x03,
    MQTT = 0x04,
    SIMPLE = 0x05
}
--websock proto
WEBSOCK_PROTO = {
    CONTINUA = 0x00,
    TEXT = 0x01,
    BINARY = 0x02,
    CLOSE = 0x08,
    PING = 0x09,
    PONG = 0x0A
}
--消息类型
MSG_TYPE = {
    STARTUP = 0x01,
    CLOSING = 0x02,
    TIMEOUT = 0x03,
    ACCEPT = 0x04,
    CONNECT = 0x05,
    HANDSHAKED = 0x06,
    RECV = 0x07,
    SEND = 0x08,
    CLOSE = 0x09,
    RECVFROM = 0x0a,
    REQUEST = 0x0b,
    RESPONSE = 0x0c
}
--任务间通信类型
REQUEST_TYPE = {
    DEF = 0x00,
    RPC = 0x01
}
--分片消息类型
SLICE_TYPE = {
    NONE = 0x00,
    START = 0x01,
    SLICE = 0x02,
    END = 0x03,
}
--超时类型
TIMEOUT_TYPE = {
    NONE = 0x00,
    NORMAL = 0x01,
    WAIT = 0x02,
}
--日志级别
LOG_LV = {
    FATAL = 0x00,
    ERROR = 0x01,
    WARN = 0x02,
    INFO = 0x03,
    DEBUG = 0x04
}
--ssl文件类型
SSLFILE_TYPE = {
    PEM = 0x01,
    ASN1 = 0x02
}
--证书验证
--NONE 不验证
--PEER 验证
--FAIL_IF_NO_PEER_CERT只能用于服务端
SSLVERIFY_TYPE = {
    NONE = 0x00,
    PEER = 0x01,
    FAIL_IF_NO_PEER_CERT = 0x03,
}

PRINT_DEBUG = true
RPC_USEJSON = true

REQUEST_TIMEOUT = 1000
NETRD_TIMEOUT = 1500
CONNECT_TIMEOUT = 3000
EVERY_EXLENS = 5
MAX_QULENS = 1024

DNS_IP = "8.8.8.8"
DNS_PORT = 53
FMT_TIME = "%H:%M:%S"
FMT_DATETIME = "%Y-%m-%d %H:%M:%S"

ERR_OK = 0
ERR_FAILED = -1
INVALID_SOCK = -1
INVALID_TNAME = 0
