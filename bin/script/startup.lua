-- 启动脚本
require("lib.define")
require("lib.utils")
require("lib.log")
local core = require("srey.core")

-- 注册需SSL证书
core.cert_register(SSL_NAME.SERVER, "ca.crt", "server.crt", "server.key")
core.p12_register(SSL_NAME.CLIENT, "client.p12", "srey")

-- 所有测试 task 注册集中在 test.test,在此调用完成注册
require("test.test")()
