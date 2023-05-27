--任务名
TASK_NAME = {
    HARBOR = 0,
    TAKS1 = 1,
    TAKS2 = 2,
    TAKS3 = 3
}
--解包类型 标识该socket类型
UNPACK_TYPE = {
    NONE = 0,
    SIMPLE = 1,
}
--组包类型
PACK_TYPE = {
    NONE = 0,
    SIMPLE = 1,
}
--user消息
USERMSG_TYPE = {
    RPC_REQUEST = 1,
    RPC_RESPONSE = 2,
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

ERR_OK = 0
ERR_FAILED = -1
INVALID_SOCK = -1
PRINT_DEBUG = true
MONITOR_TIMEOUT = true
REQUEST_TIMEOUT = 500
CONNECT_TIMEOUT = 3000
FMT_TIME = "%H:%M:%S"
FMT_DATETIME = "%Y-%m-%d %H:%M:%S"
