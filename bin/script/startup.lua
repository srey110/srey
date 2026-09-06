-- 启动脚本
require("lib.define")
require("lib.utils")
require("lib.log")
local core = require("srey.core")

-- 注册需SSL证书。证书不入 git（.gitignore 忽略 *.crt/*.key/*.p12），没跑过 bin/keys/create.sh
-- 时这两步都拿不到上下文。静默下去的话故障要等到后面 SSL listen/connect 才以"连不上"暴露，
-- 看不出根因其实是证书没生成。这两个接口在未启用 SSL 的构建里恒返回 nil，与证书缺失同一个返回值，
-- 故提示里两种成因都得写上
if not core.cert_register(SSL_NAME.SERVER, "ca.crt", "server.crt", "server.key") then
    ERROR("cert_register(server) failed: SSL disabled at build time, or run bin/keys/create.sh first.")
end
if not core.p12_register(SSL_NAME.CLIENT, "client.p12", "srey") then
    ERROR("p12_register(client) failed: SSL disabled at build time, or run bin/keys/create.sh first.")
end

-- 所有测试 task 注册集中在 test.test,在此调用完成注册
require("test.test")()
