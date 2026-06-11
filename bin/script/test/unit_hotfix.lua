-- hotfix.apply 单元测试:函数替换 / upvalue 状态保留 / helper 表字段化(方案 B)/ 各类失败路径

local srey   = require("lib.srey")
local hotfix = require("lib.hotfix")
local runner = require("test.runner")

-- 构造 inline 测试 module,注入 package.loaded;每个子段调用以重置状态
local function _setup_module()
    package.loaded.hotfix_unit_mod = nil
    local src = [[
        local M = {}
        local counter = 0
        function M.bump()
            counter = counter + 1
            return counter
        end
        function M.handle()
            return "v1"
        end
        function M._helper(x)
            return x * 2
        end
        function M.use_helper(x)
            return M._helper(x)
        end
        return M
    ]]
    local fn = assert(load(src, "=hotfix_unit_mod"))
    local mod = fn()
    package.loaded.hotfix_unit_mod = mod
    return mod
end

srey.startup(function()
runner.run("hotfix", function(t)
    -- ── 子段 1:函数替换 + 行为变更 ─────────────────────────────────
    do
        local mod = _setup_module()
        t:eq("v1", mod.handle(), "原 handle 返回 v1")
        local patch = [[
            function M.handle()
                return "v2"
            end
        ]]
        local ok = hotfix.apply("hotfix_unit_mod", patch)
        t:eq(true, ok, "apply ok")
        t:eq("v2", mod.handle(), "替换后 handle 返回 v2")
    end

    -- ── 子段 2:upvalue 状态保留(counter 跨 patch 累加,关键能力) ─────
    do
        local mod = _setup_module()
        t:eq(1, mod.bump(), "bump 初始 1")
        t:eq(2, mod.bump(), "bump 累加 2")
        -- patch 重新声明同名 local counter,upvaluejoin 接管原 UpVal
        local patch = [[
            local counter = 0
            function M.bump()
                counter = counter + 1
                return counter + 1000
            end
        ]]
        local ok = hotfix.apply("hotfix_unit_mod", patch)
        t:eq(true, ok, "apply ok")
        t:eq(1003, mod.bump(), "替换后 counter 接管原状态,3 + 1000 = 1003")
        t:eq(1004, mod.bump(), "继续累加,4 + 1000 = 1004")
    end

    -- ── 子段 3:方案 B - helper 提升为 M._helper,patch 只换 helper ───
    do
        local mod = _setup_module()
        t:eq(10, mod.use_helper(5), "原 use_helper 5 * 2 = 10")
        local patch = [[
            function M._helper(x)
                return x * 3
            end
        ]]
        local ok = hotfix.apply("hotfix_unit_mod", patch)
        t:eq(true, ok, "apply _helper ok")
        -- use_helper 未被替换,它通过 mod._helper 拿到新版,自动生效
        t:eq(15, mod.use_helper(5), "use_helper 自动用新 _helper:5 * 3 = 15")
    end

    -- ── 子段 4:apply 不存在 module → false ──────────────────────────
    do
        local ok, err = hotfix.apply("non_existing_module_xyz", "function M.x() end")
        t:eq(false, ok, "apply 不存在 module 返回 false")
        t:check(err and nil ~= err:find("not loaded"), "err 含 'not loaded'")
    end

    -- ── 子段 5:patch 语法错 → false ─────────────────────────────────
    do
        _setup_module()
        local ok, err = hotfix.apply("hotfix_unit_mod", "function M.x( bad syntax")
        t:eq(false, ok, "patch 语法错返回 false")
        t:check(err and nil ~= err:find("load"), "err 含 'load'")
    end

    -- ── 子段 6:patch 执行报错 → false ───────────────────────────────
    do
        _setup_module()
        local ok, err = hotfix.apply("hotfix_unit_mod", "error('boom')")
        t:eq(false, ok, "patch 执行错返回 false")
        t:check(err and nil ~= err:find("boom"), "err 含 'boom'")
    end

    -- ── 子段 7:patch 新增 API 不算替换 → false ─────────────────────
    do
        _setup_module()
        local ok, err = hotfix.apply("hotfix_unit_mod", "function M.brand_new() end")
        t:eq(false, ok, "patch 仅新增 API 返回 false")
        t:check(err and nil ~= err:find("no matching"), "err 含 'no matching'")
    end

    -- ── 子段 8:多函数同时替换,replaced 计数 ─────────────────────────
    do
        local mod = _setup_module()
        local patch = [[
            function M.handle() return "vN" end
            function M._helper(x) return x * 7 end
        ]]
        local ok, msg = hotfix.apply("hotfix_unit_mod", patch)
        t:eq(true, ok, "apply multi ok")
        t:check(msg and nil ~= msg:find("2"), "替换 2 个函数 msg 含 '2'")
        t:eq("vN", mod.handle(), "handle 替换")
        t:eq(35, mod.use_helper(5), "use_helper 经 mod._helper 走新版 5 * 7 = 35")
    end

    -- ── 子段 9:路径 B - patch 裸读 counter,经 patch_env metatable 转发到原 UpVal ───
    do
        local mod = _setup_module()
        mod.bump()                -- counter=1
        mod.bump()                -- counter=2
        local patch = [[
            function M.bump()
                counter = counter + 1     -- 裸读 counter,走 patch_env metatable → mod 的 counter UpVal
                return counter + 2000     -- 行为变更:加 2000 标识
            end
        ]]
        local ok = hotfix.apply("hotfix_unit_mod", patch)
        t:eq(true, ok, "apply 路径 B ok")
        t:eq(2003, mod.bump(), "路径 B:patch 裸读裸写 counter,状态接管(3 + 2000)")
        t:eq(2004, mod.bump(), "路径 B:counter 通过 UpVal 转发持续累加")
    end

    -- ── 子段 10:路径 B - patch 中读其它顶层 local(只读不写) ─────────
    do
        local mod = _setup_module()
        -- 给 mod 注入新的顶层 local 用于验证 — 用 patch 自己加
        local seed_patch = [[
            local _config = {tag = "init"}
            function M.handle() return _config.tag end
        ]]
        hotfix.apply("hotfix_unit_mod", seed_patch)
        t:eq("init", mod.handle(), "seed patch 设置 _config.tag=init")
        -- 第二次 patch 裸读 _config 应能找到上一轮的 UpVal
        local patch = [[
            function M.handle()
                return "see:" .. _config.tag
            end
        ]]
        local ok = hotfix.apply("hotfix_unit_mod", patch)
        t:eq(true, ok, "apply 二次 patch ok")
        t:eq("see:init", mod.handle(), "路径 B:patch 裸读 _config 命中 metatable 转发")
    end

    -- ── 子段 11:路径 B + 路径 A 同 patch 混用(无冲突) ──────────────
    do
        _setup_module()
        local patch = [[
            local counter   -- 路径 A:声明 local,触发 upvaluejoin 嫁接
            function M.bump()
                counter = counter + 1     -- 走 patch fn 的 upvalue(嫁接到原 UpVal)
                return counter + 3000
            end
            -- 同 chunk 同时有路径 B 风格:M.handle 裸读 counter
            function M.handle()
                return "h:" .. tostring(counter)   -- 经 patch_env metatable 转发
            end
        ]]
        local ok = hotfix.apply("hotfix_unit_mod", patch)
        t:eq(true, ok, "apply 混用 ok")
        t:eq(3001, package.loaded.hotfix_unit_mod.bump(), "路径 A bump")
        t:eq("h:1", package.loaded.hotfix_unit_mod.handle(), "路径 B handle 看到同一 counter")
    end

    -- ── 子段 12:路径 B 写 counter 后 chunk 抛错 → apply false 且 counter 已回滚(F-HF-1) ───
    do
        local mod = _setup_module()
        t:eq(1, mod.bump(), "bump 初始 1")   -- counter=1
        -- patch 裸写 counter(path-B 立即改原 UpVal)后抛错
        local patch = [[
            counter = 999
            error("boom after write")
        ]]
        local ok, err = hotfix.apply("hotfix_unit_mod", patch)
        t:eq(false, ok, "apply 执行错返回 false")
        t:check(err and nil ~= err:find("boom"), "err 含 'boom'")
        -- counter 应已回滚到 1;若未回滚则为 999,下方 bump 返回 1000
        t:eq(2, mod.bump(), "counter 回滚:1 + 1 = 2(未回滚则为 1000)")
    end

    -- ── 子段 13:patch 仅裸写 counter 无函数替换 → "no matching" 失败同样回滚(F-HF-1) ───
    do
        local mod = _setup_module()
        t:eq(1, mod.bump(), "bump 初始 1")   -- counter=1
        local ok, err = hotfix.apply("hotfix_unit_mod", "counter = 777")
        t:eq(false, ok, "纯状态写 patch 无函数替换返回 false")
        t:check(err and nil ~= err:find("no matching"), "err 含 'no matching'")
        t:eq(2, mod.bump(), "counter 回滚:1 + 1 = 2(未回滚则为 778)")
    end
end)
end)
