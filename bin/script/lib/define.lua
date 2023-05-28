--任务名
TASK_NAME = {
    HARBOR = 0,
    TAKS1 = 1,
    TAKS2 = 2,
    TAKS3 = 3
}
--解包类型 标识该socket类型 与c unpack_type对应
UNPACK_TYPE = {
    NONE = 0,
    RPC = 1,
    HTTP = 2,
    SIMPLE = 3
}
--组包类型 与c pack_type对应
PACK_TYPE = {
    NONE = 0,
    RPC = 1,
    SIMPLE = 2,
}
--任务间消息
TASKMSG_TYPE = {
    REQUEST = 1,
    RESPONSE = 2,
    NETREQ = 3,
    NETRESP = 4,
}
--ssl文件类型
SSLFILE_TYPE = {
    PEM = 1,
    ASN1 = 2
}
--日志级别
LOG_LV = {
    FATAL = 0,
    ERROR = 1,
    WARN = 2,
    INFO = 3,
    DEBUG = 4
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
