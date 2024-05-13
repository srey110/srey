TASK_NAME = {
    HARBOR = 0x01,
    --test
    startup_closing = 1000,
    timeout = 1001,
    comm1 = 1002,
    comm2 = 1003,
    tcp = 1004,
    ssl = 1005,
    udp = 1006,
    syn = 1007,
    netharbor = 1008
}
--SSL名
SSL_NAME = {
    NONE =   0x00,
    SERVER = 0x01,
    CLIENT = 0x02
}
--ssl文件类型
SSLFILE_TYPE = {
    PEM =  0x01,
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
--事件
APPEND_EV = {
    ACCEPT =  0x01,
    CONNECT = 0x02,
    CLOSE =   0x04,
    SEND =    0x08
}
--包类型 标识该socket类型 与c pack_type对应
PACK_TYPE = {
    NONE =    0x00,
    HTTP =    0x01,
    WEBSOCK = 0x02,
    SIMPLE =  0x03
}
--websock proto
WEBSOCK_PROTO = {
    CONTINUA = 0x00,
    TEXT =     0x01,
    BINARY =   0x02,
    CLOSE =    0x08,
    PING =     0x09,
    PONG =     0x0A
}
--分片消息类型
SLICE_TYPE = {
    NONE =  0x00,
    START = 0x01,
    SLICE = 0x02,
    END =   0x03,
}

ERR_OK = 0
ERR_FAILED = -1
INVALID_SOCK = -1
INVALID_TNAME = 0
