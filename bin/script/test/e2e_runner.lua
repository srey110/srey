-- e2e 自动化 runner：用 srey.popen 起 python3 bin/py_assist/test_*.py 子进程，
-- 等 server_http/ws/mqtt task 就绪（startup.lua 已先于它们注册，但 listen 是异步）后串行跑，
-- 把每个脚本的 exit code 转成 npass/nfail，向 reporter 上报为单一模块 "e2e"。

local srey   = require("lib.srey")
local runner = require("test.runner")
local core   = require("srey.core")
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
}
-- ssl_reneg 连 15443，没编 SSL 时 server_http 起不了那个监听
if core.with_ssl() then
    _SCRIPTS[#_SCRIPTS + 1] = "ssl_reneg"
end

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
            -- 边等边抽,不能只 waitexit 完再读：popen2.c 的管道是 socketpair(AF_UNIX, SOCK_STREAM)
            -- 且不设 SO_SNDBUF,读端不抽时 python 输出填满缓冲(macOS 约 8KB)就阻塞在 write 永不退出,
            -- 症状是 waitexit 超时而真因是这边没读。超时分支也得把已抽到的打出来——它是唯一的线索
            local chunks = {}
            local t0 = srey.timer_ms()
            local exited = false
            local chunk
            while true do
                if ctx:waitexit(0) then
                    exited = true
                    break
                end
                if srey.timer_ms() - t0 >= _PY_TIMEOUT_MS then
                    break
                end
                chunk = ctx:read(64 * 1024)
                if chunk and #chunk > 0 then
                    chunks[#chunks + 1] = chunk
                else
                    srey.sleep(50)
                end
            end
            chunk = ctx:read(256 * 1024)
            if chunk and #chunk > 0 then
                chunks[#chunks + 1] = chunk
            end
            local output = table.concat(chunks)
            io.write(output)
            if "" ~= output and "\n" ~= output:sub(-1) then
                io.write("\n")
            end
            io.write("\n")
            io.stdout:flush()
            if exited then
                local code = ctx:exitcode()
                t:check(0 == code, name .. ": python exit code " .. tostring(code))
            else
                t:fail(name .. ": python timeout " .. _PY_TIMEOUT_MS .. "ms")
            end
            -- 两条分支都要 close：ctx 只是个 local，回收全靠 __gc，而这个循环几乎不分配、
            -- GC 未必在下一轮前触发，四条 socketpair fd 会一直攥到 e2e 阶段结束
            ctx:close()
            ::continue::
        end
    end)
end)
