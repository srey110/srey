local log = require("lib.log")
local core = require("lib.core")
require("lib.funcs")

core.evssl_new(SSL_NAME.SERVER, "ca.crt", "sever.crt", "sever.key", SSLFILE_TYPE.PEM)
core.evssl_p12new(SSL_NAME.CLIENT, "client.p12", "srey")

core.task_register("test.test1", TASK_NAME.TEST1)
core.task_register("test.test2", TASK_NAME.TEST2)
core.task_register("test.test4", TASK_NAME.TEST4)
core.task_register("test.test_wbsk", TASK_NAME.TEST_WBSK)
core.task_register("test.test_http", TASK_NAME.TEST_HTTP)

log.FATAL("FATAL")
local url = "ftp://user:psw@127.0.0.1:8080/path/file?a=1&b=2#anchor"
printd(dump(core.url_parse(url)))
