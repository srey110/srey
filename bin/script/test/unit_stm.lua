-- srey.stm 绑定层单元测试: writer / reader 配对 + __call (update/read) + __gc 自动 release
-- 单 lua_State 内同时持 writer 和 reader, 通过 stm.copy + stm.newcopy 模拟跨 task handle 传递

local srey   = require("lib.srey")
local stm    = require("srey.stm")
local utils  = require("srey.utils")
local runner = require("test.runner")

srey.startup(function()
runner.run("stm", function(t)
    -- ── 1) 基础: new + copy + newcopy + read 首次拿到 true + 数据
    do
        local w = stm.new("hello")
        local handle = stm.copy(w)
        local r = stm.newcopy(handle)
        local got
        local updated = r(function(lud, sz)
            got = utils.ud_str(lud, sz)
        end)
        t:eq(true, updated, "首次 read updated=true")
        t:eq("hello", got, "读到 hello")
    end
    collectgarbage("collect")

    -- ── 2) update 后再 read 拿到新版 + updated=true
    do
        local w = stm.new("ver1")
        local r = stm.newcopy(stm.copy(w))
        local got1, got2
        r(function(lud, sz) got1 = utils.ud_str(lud, sz) end)
        w("ver2")
        local updated = r(function(lud, sz) got2 = utils.ud_str(lud, sz) end)
        t:eq("ver1", got1, "首次 read ver1")
        t:eq(true, updated, "update 后 updated=true")
        t:eq("ver2", got2, "更新后读 ver2")
    end
    collectgarbage("collect")

    -- ── 3) lastcopy 跳过: 同一快照连续读, 第 2 次 updated=false, func 不被调用
    do
        local w = stm.new("same")
        local r = stm.newcopy(stm.copy(w))
        local count = 0
        local upd1 = r(function() count = count + 1 end)
        local upd2 = r(function() count = count + 1 end)
        t:eq(true, upd1, "第 1 次 updated=true")
        t:eq(false, upd2, "第 2 次 updated=false (lastcopy 相同)")
        t:eq(1, count, "func 只调用 1 次")
    end
    collectgarbage("collect")

    -- ── 4) 多 reader: 各自独立 lastcopy 状态, 互不串扰
    do
        local w = stm.new("multi")
        local r1 = stm.newcopy(stm.copy(w))
        local r2 = stm.newcopy(stm.copy(w))
        local s1, s2
        r1(function(lud, sz) s1 = utils.ud_str(lud, sz) end)
        r2(function(lud, sz) s2 = utils.ud_str(lud, sz) end)
        t:eq("multi", s1, "r1 读到 multi")
        t:eq("multi", s2, "r2 读到 multi")
        -- update: 两个 reader 各自的 lastcopy 都还是旧的, 下次读都应 updated=true
        w("multi2")
        local upd1 = r1(function(lud, sz) s1 = utils.ud_str(lud, sz) end)
        local upd2 = r2(function(lud, sz) s2 = utils.ud_str(lud, sz) end)
        t:eq(true, upd1, "r1 update 后 updated=true")
        t:eq(true, upd2, "r2 update 后 updated=true")
        t:eq("multi2", s1, "r1 读到 multi2")
        t:eq("multi2", s2, "r2 读到 multi2")
    end
    collectgarbage("collect")

    -- ── 5) writer 退出后 reader 状态: 旧 lastcopy 仍可读; 新 stm_grab_data 返 NULL → updated=false
    do
        local w = stm.new("survive")
        local r = stm.newcopy(stm.copy(w))
        local got
        r(function(lud, sz) got = utils.ud_str(lud, sz) end)
        t:eq("survive", got, "writer 在: 首次读 survive")
        -- 主动让 writer 出作用域 + 强制 GC 触发 __gc → stm_free
        w = nil
        collectgarbage("collect")
        -- 之后 r 调 read: ctx->data 已被 writer 清掉, stm_grab_data 返 NULL, updated=false
        local count = 0
        local upd = r(function() count = count + 1 end)
        t:eq(false, upd, "writer 退出后 read updated=false")
        t:eq(0, count, "func 不被调用")
    end
    collectgarbage("collect")

    -- ── 6) read 的可选 ud 参数透传给 func 作为第 3 个实参
    do
        local w = stm.new("payload")
        local r = stm.newcopy(stm.copy(w))
        local cap_lud, cap_sz, cap_ud
        local upd = r(function(lud, sz, ud)
            cap_lud, cap_sz, cap_ud = lud, sz, ud
        end, "user_ctx")
        t:eq(true, upd, "updated=true")
        t:eq("payload", utils.ud_str(cap_lud, cap_sz), "func 收到 lud + sz")
        t:eq("user_ctx", cap_ud, "func 收到透传的 ud=user_ctx")
    end
    collectgarbage("collect")

    -- ── 7) read 透传 func 返回值: r 返回 (boolean, ...func_ret)
    do
        local w = stm.new("ret")
        local r = stm.newcopy(stm.copy(w))
        local upd, a, b = r(function() return 42, "hello" end)
        t:eq(true, upd, "首位 boolean true")
        t:eq(42, a, "func 第 1 返回值")
        t:eq("hello", b, "func 第 2 返回值")
    end
    collectgarbage("collect")

    -- ── 8) stm.copy 类型校验: nil/错类型/非 writer userdata 须报错(luaL_argcheck)而非解引用崩溃
    do
        local w = stm.new("typecheck")
        local r = stm.newcopy(stm.copy(w))
        t:eq(false, pcall(stm.copy),      "stm.copy() 无参应报错")
        t:eq(false, pcall(stm.copy, nil), "stm.copy(nil) 应报错")
        t:eq(false, pcall(stm.copy, {}),  "stm.copy(table) 应报错")
        t:eq(false, pcall(stm.copy, "x"), "stm.copy(string) 应报错")
        t:eq(false, pcall(stm.copy, r),   "stm.copy(reader) 错类型 userdata 应报错")
    end
    collectgarbage("collect")

    -- ── 9) 回调内重入读同一 reader:外层 lud 不被重入释放(UAF 回归,ASan 下判别)
    do
        local w = stm.new("v1")
        local r = stm.newcopy(stm.copy(w))
        local outer
        r(function(lud, sz)
            w("v2")              -- update:旧快照 ref 减到仅外层 read 持有
            r(function() end)    -- 重入读同一 reader
            outer = utils.ud_str(lud, sz)   -- 重入返回后外层 lud 须仍有效
        end)
        t:eq("v1", outer, "reentry: 重入读后外层 lud 仍指向有效 v1 快照")
    end
    collectgarbage("collect")

    -- ── 10) 回调抛错:lua_pcall 后引用正确交接,错误仍向上传播,不泄漏 snap(退出期 _memcheck/ASan 验)
    do
        local w = stm.new("e1")
        local r = stm.newcopy(stm.copy(w))
        local ok = pcall(function() r(function() error("boom") end) end)
        t:eq(false, ok, "callback-error: 回调抛错仍向上传播(被 pcall 捕获)")
        w("e2")
        local got
        r(function(lud, sz) got = utils.ud_str(lud, sz) end)
        t:eq("e2", got, "callback-error: 抛错后 reader 仍能正常读新快照")
    end
    collectgarbage("collect")
end)
end)
