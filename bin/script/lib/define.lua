--任务名
TASK_NAME = {
    HARBOR = 0x00,
    TAKS1 = 0x01,
    TAKS2 = 0x02,
    TAKS3 = 0x03
}
--解包类型 标识该socket类型 与c unpack_type对应
UNPACK_TYPE = {
    NONE = 0x00,
    RPC = 0x01,
    HTTP = 0x02,
    SIMPLE = 0x03
}
--组包类型 与c pack_type对应
PACK_TYPE = {
    NONE = 0x00,
    RPC = 0x01,
    SIMPLE = 0x02,
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

REQUEST_TIMEOUT = 500
NETREQ_TIMEOUT = 1000
CONNECT_TIMEOUT = 3000

FMT_TIME = "%H:%M:%S"
FMT_DATETIME = "%Y-%m-%d %H:%M:%S"

NETRPC_TIMEDIFF = 60
NETRPC_SIGNKEY = "x3njVVstBXMAxNdxNbKINeBnS9fVyoR6"

ERR_OK = 0
ERR_FAILED = -1
INVALID_SOCK = -1
