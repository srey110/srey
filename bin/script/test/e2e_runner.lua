-- e2e 自动化 runner：用 srey.popen 起 python3 bin/py_assist/test_*.py 子进程，
-- 等 server_http/ws/mqtt task 就绪（startup.lua 已先于它们注册，但 listen 是异步）后串行跑，
-- 把每个脚本的 exit code 转成 npass/nfail，向 reporter 上报为单一模块 "e2e"。

local srey   = require("lib.srey")
local runner = require("test.runner")
local popen  = require("srey.popen")

local _PY_TIMEOUT_MS = 60 * 1000

-- python 测试脚本名（test_<name>.py），位于 _propath/py_assist/ 下。
-- 与 test/main.c 的 pyitems 不是同一份：ssl_reneg 要 15443 的 SSL 端口，只有本侧的
-- server_http.lua 起了它，C 侧 task_http_server.c 没有 SSL 监听，故那边跑不了。
-- 反过来 mixed 两侧都跑得起来，两边都列上
local _SCRIPTS = {
    "http",
    "ws",
    "mqtt",
    "mixed",
    "ssl_reneg",
}

-- 用 _propath（C 层注入的程序根路径）拼绝对路径，避免依赖 cwd
local _PY_DIR = _propath .. _pathsep .. "py_assist" .. _pathsep

srey.startup(function()
    -- 给 server task 一点时间完成 srey.listen（startup 是 coroutine 化的，需要让出）
    srey.sleep(1000)
    runner.run(function(t)
        for _, name in ipairs(_SCRIPTS) do
            local cmd = "python3 " .. _PY_DIR .. "test_" .. name .. ".py 2>&1"
            -- 仿 C 层 main.c LOG_INFO("running %s", pycmd) + 后续 PRINT outbuf 的格式：
            -- 直接 io.write 到 stdout，不走 log；python 输出原样打印
            printd("running %s", cmd)
            local ctx = popen.new(cmd, "r")
            if not ctx then
                t:fail("popen.new " .. cmd)
                goto continue
            end
            if not ctx:waitexit(_PY_TIMEOUT_MS) then
                t:fail(name .. ": python timeout " .. _PY_TIMEOUT_MS .. "ms")
                ctx:close()
                goto continue
            end
            local code = ctx:exitcode()
            local output = ctx:read(256 * 1024)
            io.write(output)
            if "\n" ~= output:sub(-1) then
                io.write("\n")
            end
            io.write("\n")
            io.stdout:flush()
            t:check(0 == code, name .. ": python exit code " .. tostring(code))
            -- 成功分支也要 close：ctx 只是个 local，回收全靠 __gc，而这个循环几乎不分配、
            -- GC 未必在下一轮前触发，四条 socketpair fd 会一直攥到 e2e 阶段结束
            ctx:close()
            ::continue::
        end
    end)
end)
