--任务名
TASK_NAME = {
    HARBOR = 0x00,
    TASK1 = 0x01,
    TASK2 = 0x02,
    TASK3 = 0x03,
    TASK4 = 0x04
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

RPCREQ_TIMEOUT = 500
NETRD_TIMEOUT = 3000
CONNECT_TIMEOUT = 3000

FMT_TIME = "%H:%M:%S"
FMT_DATETIME = "%Y-%m-%d %H:%M:%S"

ERR_OK = 0
ERR_FAILED = -1
INVALID_SOCK = -1
