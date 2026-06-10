-- e2e 自动化 runner：用 srey.popen 起 python3 bin/py_assist/test_*.py 子进程，
-- 等 server_http/ws/mqtt task 就绪（startup.lua 已先于它们注册，但 listen 是异步）后串行跑，
-- 把每个脚本的 exit code 转成 npass/nfail，向 reporter 上报为单一模块 "e2e"。

local srey   = require("lib.srey")
local runner = require("test.runner")
local popen  = require("srey.popen")

local _PY_TIMEOUT_MS = 60 * 1000

-- python 测试脚本名（test_<name>.py），位于 _propath/py_assist/ 下
local _SCRIPTS = {
    "http",
    "ws",
    "mqtt",
}

-- 用 _propath（C 层注入的程序根路径）拼绝对路径，避免依赖 cwd
local _PY_DIR = _propath .. _pathsep .. "py_assist" .. _pathsep

srey.startup(function()
    -- 给 server task 一点时间完成 srey.listen（startup 是 coroutine 化的，需要让出）
    srey.sleep(1000)
    runner.run("e2e", function(t)
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
            ::continue::
        end
    end)
end)
