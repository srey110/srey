TASK_NAME = {
    NONE =   0x00,
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
    netharbor = 1008,
    wbskd = 1009,
    wbsksv = 1010,
    httpd = 1011,
    httpsv = 1012,
    mysql = 1013
}
MYSQL_PACK_TYPE =  {
    MPACK_OK = 0x00,
    MPACK_ERR = 0x01,
    MPACK_QUERY = 0x02,
    MPACK_STMT_PREPARE = 0x03,
    MPACK_STMT_EXECUTE = 0x04
}
--SSL名
SSL_NAME = {
    NONE =   0x00,
    SERVER = 0x01,
    CLIENT = 0x02
}
--ssl文件类型
SSLFILE_TYPE = {
    PEM  = 0x01,
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
NET_EV = {
    NONE    = 0x00,
    ACCEPT  = 0x01,
    AUTHSSL = 0x02,
    SEND    = 0x04
}
--包类型 标识该socket类型 与c pack_type对应
PACK_TYPE = {
    NONE    = 0x00,
    HTTP    = 0x01,
    WEBSOCK = 0x02,
    REDIS   = 0x03,
    MYSQL   = 0x04,
    CUSTZ   = 0x05
}

ERR_OK = 0
ERR_FAILED = -1
INVALID_SOCK = -1
