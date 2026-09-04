-- 测试 task 注册：集中注册所有 unit/db/server 测试 task 与 reporter。
-- 由 startup.lua 在公共初始化(lib.define/utils/log + SSL 证书)后调用 register()。
-- 不依赖集成环境的 unit 测试 + 依赖 docker 的 db 测试 + reporter 汇总。

local task = require("srey.task")

-- 上报 reporter 的测试 task 列表，注册顺序即此顺序。每项 { 脚本, 名字 [, 额外注册参数...] }。
-- 第 2 列同时是 task 名与上报名：注册用它，reporter 的期望列表用它，runner 也从 task 名反读它
-- （见 runner.run），所以名字全仓只此一处，三者对不上在结构上就不可能。
-- 它与脚本名有意不同（unit_bson 的 task 叫 bson），汇总表因此不必满屏 unit_ 前缀。
-- 新增/删除模块只改这里一处
local TESTS = {
    -- 不需要集成环境
    { "test.unit_bson",           "bson" },
    { "test.unit_crypt",          "crypt" },
    { "test.unit_utils",          "utils" },
    { "test.unit_protocol",       "protocol" },
    { "test.unit_mqtt",           "mqtt" },
    { "test.lua_layer",           "lua_layer" },
    { "test.unit_db_bind",        "db_bind" },
    { "test.unit_framework",      "framework" },
    { "test.unit_lib",            "lib" },
    { "test.unit_router",         "unit_router" },
    { "test.unit_fork",           "fork" },
    { "test.unit_coro",           "coro" },
    { "test.unit_serial",         "serial" },
    { "test.unit_yyjson",         "yyjson" },
    { "test.unit_multicast",      "multicast" },
    { "test.unit_udp_multicast",  "udp_multicast" },
    { "test.unit_multi_call",     "multi_call" },
    { "test.unit_hotfix",         "hotfix" },
    { "test.unit_inject",         "inject" },
    { "test.unit_debug",          "debug_req" },
    { "test.unit_seri",           "seri" },
    { "test.unit_stm",            "stm" },
    { "test.unit_kcp",            "kcp" },
    { "test.unit_http",           "http_client" },
    { "test.unit_websock",        "websock_client" },
    -- 同一 task 内起假 SMTP 服务端再用 4 个协程并发投递,验证 smtp.lua 的命令串行化
    -- (镜像 C 层 task_smtp.c 的同名用例);不依赖外网账号,末参为监听端口
    { "test.smtp_fake",           "smtp_fake", 12526 },
    { "test.e2e_runner",          "e2e" },-- 用 srey.popen 跑 bin/py_assist/test_*.py 自动收集 exit code
    -- 集成测试(依赖 docker-compose)
    { "test.db_mysql",            "db_mysql" },
    { "test.db_pgsql",            "db_pgsql" },
    { "test.db_redis",            "db_redis" },
    { "test.db_mongo",            "db_mongo" },
}

-- 注册 reporter + 各测试 task。task 名取 TESTS 的第 2 列。
local function register()
    printd("[runner] %d tests registered", #TESTS)

    -- reporter 首先注册,把期望名单一并传过去:它据此判断收齐没有,超时兜底时还能报出
    -- 具体是谁没上报——只传一个计数的话，缺模块只会表现为"永远不出汇总"
    local reports = {}
    for i, e in ipairs(TESTS) do
        reports[i] = e[2]
    end
    task.register("test.reporter", "reporter", 0, table.unpack(reports))

    -- 先注册 3 个 subscriber,确保 multi_call 启动时它们的 on_requested 已挂上。
    -- 第 4 个实参是 publisher 的 task 名,即 TESTS 里 unit_multi_call 那行的第 2 列
    task.register("test.multi_call_sub", "multi_call_sub_a", 0, "multi_call", 1)
    task.register("test.multi_call_sub", "multi_call_sub_b", 0, "multi_call", 2)
    task.register("test.multi_call_sub", "multi_call_sub_c", 0, "multi_call", 3)

    for _, e in ipairs(TESTS) do
        task.register(e[1], e[2], 0, table.unpack(e, 3))
    end

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
