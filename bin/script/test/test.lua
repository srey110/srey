-- 测试 task 注册：集中注册所有 unit/db/server 测试 task 与 reporter。
-- 由 startup.lua 在公共初始化(lib.define/utils/log + SSL 证书)后调用 register()。
-- 不依赖集成环境的 unit 测试 + 依赖 docker 的 db 测试 + reporter 汇总。

local task = require("srey.task")

-- 上报 reporter 的测试 task 列表(顺序无关)。新增/删除模块时同步更新,expected 自动重算。
local TESTS = {
    -- 不需要集成环境
    "test.unit_bson",
    "test.unit_crypt",
    "test.unit_utils",
    "test.unit_protocol",
    "test.unit_mqtt",
    "test.lua_layer",
    "test.unit_db_bind",
    "test.unit_framework",
    "test.unit_lib",
    "test.unit_protoc",
    "test.unit_router",
    "test.unit_fork",
    "test.unit_serial",
    "test.unit_multicast",
    "test.unit_udp_multicast",
    "test.unit_multi_call",
    "test.unit_dc_client",
    "test.unit_sc_client",
    "test.unit_hotfix",
    "test.unit_inject",
    "test.unit_seri",
    "test.unit_stm",
    "test.e2e_runner",   -- 用 srey.popen 跑 bin/py_assist/test_*.py 自动收集 exit code
    -- 集成测试(依赖 docker-compose)
    "test.db_mysql",
    "test.db_pgsql",
    "test.db_redis",
    "test.db_mongo",
}

-- 注册 reporter + 各测试 task。task 名用与脚本同名的字符串("test.xxx" → name "xxx")。
local function register()
    local expected = #TESTS
    printd("[runner] %d tests registered", expected)

    -- reporter 首先注册,期望模块数 = TESTS 长度
    task.register("test.reporter", "reporter", 0, expected)

    task.register("test.unit_bson", "unit_bson", 0)
    task.register("test.unit_crypt", "unit_crypt", 0)
    task.register("test.unit_utils", "unit_utils", 0)
    task.register("test.unit_protocol", "unit_protocol", 0)
    task.register("test.unit_mqtt", "unit_mqtt", 0)
    task.register("test.lua_layer", "lua_layer", 0)
    task.register("test.unit_db_bind", "unit_db_bind", 0)
    task.register("test.unit_framework", "unit_framework", 0)
    task.register("test.unit_lib", "unit_lib", 0)
    task.register("test.unit_protoc", "unit_protoc", 0)
    task.register("test.unit_router", "unit_router", 0)
    task.register("test.unit_fork", "unit_fork", 0)
    task.register("test.unit_serial", "unit_serial", 0)
    task.register("test.unit_multicast", "unit_multicast", 0)
    task.register("test.unit_udp_multicast", "unit_udp_multicast", 0)
    -- 先注册 3 个 subscriber,确保 unit_multi_call 启动时它们的 on_requested 已挂上
    task.register("test.multi_call_sub", "multi_call_sub_a", 0, "unit_multi_call", 1)
    task.register("test.multi_call_sub", "multi_call_sub_b", 0, "unit_multi_call", 2)
    task.register("test.multi_call_sub", "multi_call_sub_c", 0, "unit_multi_call", 3)
    task.register("test.unit_multi_call", "unit_multi_call", 0)
    task.register("test.unit_dc_client", "unit_dc_client", 0)
    task.register("test.unit_sc_client", "unit_sc_client", 0)
    task.register("test.unit_hotfix", "unit_hotfix", 0)
    task.register("test.unit_inject", "unit_inject", 0)
    task.register("test.unit_seri", "unit_seri", 0)
    task.register("test.unit_stm", "unit_stm", 0)
    task.register("test.e2e_runner", "e2e_runner", 0)
    task.register("test.db_mysql", "db_mysql", 0)
    task.register("test.db_pgsql", "db_pgsql", 0)
    task.register("test.db_redis", "db_redis", 0)
    task.register("test.db_mongo", "db_mongo", 0)

    -- 长驻 e2e 服务端 task:bin/py_assist/test_*.py 直连测试用,不上报 reporter
    task.register("test.server_http", "server_http", 0)
    task.register("test.server_ws",   "server_ws", 0)
    task.register("test.server_mqtt", "server_mqtt", 0)

    -- SMTP 客户端测试(仿照 test/task_smtp.c 参数风格)。
    -- 默认禁用:用户名/密码/邮箱地址需根据实际邮箱服务填写后再去掉注释。
    -- task.register("test.smtp_client", "smtp_client", 0,
    --               SSL_NAME.CLIENT, "smtp.gmail.com", 465,
    --               "your-account@gmail.com", "your-app-password",
    --               "your-account@gmail.com", "recipient1@example.com", "recipient2@example.com",
    --               "")
end

return register
