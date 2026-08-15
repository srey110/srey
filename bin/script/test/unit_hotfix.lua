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

-- 带 chunk-local function 的测试 module(与上面那个隔离,免得多出的函数影响既有子段)。
-- tag 是状态型 local,用来顺带确认类型判据没误伤路径 A 的嫁接
local function _setup_localfn_module()
    package.loaded.hotfix_localfn_mod = nil
    local src = [[
        local M = {}
        local tag = "T"
        local function _fmt(x)
            return "old:" .. x
        end
        function M.render(x)
            return _fmt(x) .. tag
        end
        return M
    ]]
    local fn = assert(load(src, "=hotfix_localfn_mod"))
    local mod = fn()
    package.loaded.hotfix_localfn_mod = mod
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

    -- ── 子段 12b:patch 写出的真全局同样要回滚 ────────────────────────────
    -- 未命中 upmap 的裸写落到 _G,旧版不记撤销日志 → chunk 抛错后 module upvalue 还原了,
    -- 写出去的全局却永久留在该 task 的 _G 里,调用方以为"原状态未被污染"
    do
        _setup_module()
        _G.hf_probe_flag = nil
        _G.hf_probe_keep = "orig"
        local patch = [[
            hf_probe_flag = 1
            hf_probe_keep = "patched"
            error("boom after global write")
        ]]
        local ok = hotfix.apply("hotfix_unit_mod", patch)
        t:eq(false, ok, "apply 执行错返回 false")
        t:eq(nil, _G.hf_probe_flag, "原本不存在的全局回滚为 nil")
        t:eq("orig", _G.hf_probe_keep, "原本有值的全局回滚为原值")
        _G.hf_probe_keep = nil
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

    -- ── 子段 14:apply 缺 source(nil / 非 string)→ false,不抛错 ─────
    do
        _setup_module()
        local ok, err = hotfix.apply("hotfix_unit_mod")
        t:eq(false, ok, "缺 source 返回 false")
        t:check(err and nil ~= err:find("source"), "err 含 'source'")
        local ok2, err2 = hotfix.apply("hotfix_unit_mod", 123)
        t:eq(false, ok2, "source 非 string(number)返回 false")
        t:check(err2 and nil ~= err2:find("source"), "err 含 'source'")
    end

    -- ── 子段 15:同名遮蔽 upvalue(同名不同 cell)→ 补丁碰到那个名字才拒绝 ─────
    do
        package.loaded.hotfix_shadow_mod = nil
        -- 两个 do 块各声明同名 local v,M.f1/M.f2 捕获不同 cell;按名嫁接无法判定目标
        local src = [[
            local M = {}
            do local v = 1; function M.f1() return v end end
            do local v = 2; function M.f2() return v end end
            return M
        ]]
        -- 每个用例都重建:f1 一旦被换成不捕获 v 的新版,同名遮蔽就只剩一个 cell,后面再测就测了个空
        local function _fresh()
            package.loaded.hotfix_shadow_mod = assert(load(src, "=hotfix_shadow_mod"))()
        end
        -- 补丁不碰 v:遮蔽与这次热修无关,照常替换
        _fresh()
        local ok0 = hotfix.apply("hotfix_shadow_mod", "function M.f1() return 9 end")
        t:eq(true, ok0, "补丁不碰遮蔽名字时照常热修")
        t:eq(9, package.loaded.hotfix_shadow_mod.f1(), "不碰遮蔽名字的替换已生效")
        -- 路径 A:补丁声明同名 local,嫁接前的整体校验挡下
        _fresh()
        local ok1, err1 = hotfix.apply("hotfix_shadow_mod", "local v; function M.f1() return v end")
        t:eq(false, ok1, "补丁声明同名遮蔽 upvalue → 拒绝")
        t:check(err1 and nil ~= err1:find("shadowed"), "err 含 'shadowed'")
        t:eq(1, package.loaded.hotfix_shadow_mod.f1(), "拒绝后原函数未被半改")
        -- 路径 B:补丁在 chunk 顶层裸读同名变量,env 转发时撞上
        _fresh()
        local ok2, err2 = hotfix.apply("hotfix_shadow_mod", "local x = v; function M.f2() return x end")
        t:eq(false, ok2, "补丁裸读同名遮蔽 upvalue → 拒绝")
        t:check(err2 and nil ~= err2:find("shadowed"), "err 含 'shadowed'")
        package.loaded.hotfix_shadow_mod = nil
    end

    -- ── 子段 16:patch 为预编译字节码 → load 't' 模式拒绝(禁止注入字节码)──
    do
        _setup_module()
        local bytecode = string.dump(function() return 1 end)
        local ok, err = hotfix.apply("hotfix_unit_mod", bytecode)
        t:eq(false, ok, "字节码 patch 被拒绝")
        t:check(err and nil ~= err:find("binary"), "err 含 'binary'(被 't' 模式拒绝)")
    end

    -- ── 子段 17(已知限制 #3):path-B 对独占 cell 二次热修失败,反复迭代须用 path-A ──
    -- _setup_module 中 counter 仅 bump 引用;首次 path-B 替换后 mod.bump 只剩 _ENV、不再具名持有
    -- counter,二次 path-B 时 _collect_upvalues 扫不到 cell → env 转发回退 _G → nil → 运行报错。
    do
        local mod = _setup_module()
        mod.bump()   -- counter=1
        t:eq(true, hotfix.apply("hotfix_unit_mod", [[
            function M.bump() counter = counter + 1; return counter + 100 end
        ]]), "首次 path-B apply ok")
        t:eq(102, mod.bump(), "首次 path-B 生效:2 + 100")
        -- 二次 path-B:apply 本身成功(仅替换函数),但新 bump 运行时 counter 解析为 _G.counter=nil
        t:eq(true, hotfix.apply("hotfix_unit_mod", [[
            function M.bump() counter = counter + 1; return counter + 200 end
        ]]), "二次 path-B apply 仍返 true(只替换函数)")
        t:eq(false, pcall(mod.bump), "已知限制:二次 path-B 的 bump 运行报错(counter 丢失→nil+1)")
    end

    -- ── 子段 18(限制条件性):counter 被其他未替换函数持有时,二次 path-B 仍能定位 cell ──
    do
        package.loaded.hotfix_shared_uv = nil
        local src = [[
            local M = {}
            local counter = 0
            function M.bump() counter = counter + 1; return counter end
            function M.peek() return counter end   -- 同样持有 counter,本测试不替换它
            return M
        ]]
        package.loaded.hotfix_shared_uv = assert(load(src, "=hotfix_shared_uv"))()
        local m = package.loaded.hotfix_shared_uv
        m.bump()   -- counter=1
        t:eq(true, hotfix.apply("hotfix_shared_uv", [[
            function M.bump() counter = counter + 1; return counter + 10 end
        ]]), "首次 path-B apply ok")
        t:eq(12, m.bump(), "首次:2 + 10")
        -- 二次 path-B:mod.bump 已无具名 counter,但 mod.peek 仍持有 → _collect_upvalues 命中
        t:eq(true, hotfix.apply("hotfix_shared_uv", [[
            function M.bump() counter = counter + 1; return counter + 20 end
        ]]), "二次 path-B apply ok")
        t:eq(23, m.bump(), "二次成功:peek 仍持 counter cell,3 + 20")
        package.loaded.hotfix_shared_uv = nil
    end

    -- ── 子段 19:patch 重新声明同名 local function,新实现必须生效(约束 4)──
    -- 修复前 _join_upvalues 只按名嫁接不看值类型,把 patch 的 _fmt 槽接回原 module 的 cell,
    -- patch 里新写的 _fmt 成了死代码 → 行为一点没变,而 apply 照样返回 true + "[OK]"
    do
        local mod = _setup_localfn_module()
        t:eq("old:1T", mod.render(1), "原 render 用旧 _fmt")
        local patch = [[
            local function _fmt(x)
                return "new:" .. x
            end
            function M.render(x)
                return _fmt(x) .. tag
            end
        ]]
        local ok = hotfix.apply("hotfix_localfn_mod", patch)
        t:eq(true, ok, "apply ok")
        -- new: 证明 patch 的 _fmt 生效;尾部 T 证明状态型 local 的路径 B 转发没被类型判据误伤
        t:eq("new:1T", mod.render(1), "patch 的 _fmt 生效且 tag 仍解析到原 cell")
        package.loaded.hotfix_localfn_mod = nil
    end

    -- ── 子段 19b:只被非导出 helper 捕获的模块级 local ──────────────────────
    -- pre 不是 mod 表的值,也不被任何导出函数直接捕获,只有 local function _fmt 持有它。
    -- 扫描不往函数型 upvalue 里递归的话,upmap 里根本没有 pre:patch 裸读它会一路回退到
    -- _G 拿 nil,裸写则落到 _G,而 apply 照样报"替换成功"——操作者以为热更生效了
    do
        package.loaded.hotfix_deepup_mod = nil
        local src = [[
            local M = {}
            local pre = "old:"
            local function _fmt(x)
                return pre .. x
            end
            function M.render(x)
                return _fmt(x)
            end
            function M.render2(x)
                return _fmt(x)
            end
            return M
        ]]
        local mod = assert(load(src, "=hotfix_deepup_mod"))()
        package.loaded.hotfix_deepup_mod = mod
        t:eq("old:1", mod.render(1), "原 render")
        -- 读:必须拿到模块里那份 "old:",不是 _G 的 nil
        local ok = hotfix.apply("hotfix_deepup_mod", [[
            function M.render(x)
                return "[" .. pre .. x .. "]"
            end
        ]])
        t:eq(true, ok, "apply ok(读)")
        local rok, rv = pcall(mod.render, 1)
        t:check(rok and "[old:1]" == rv, "patch 读到只被 _fmt 捕获的 pre: " .. tostring(rv))
        -- 写:必须落到同一个 cell。render2 没被替换、仍走原 _fmt,它看得到才算真的写对了
        ok = hotfix.apply("hotfix_deepup_mod", [[
            function M.render(x)
                pre = "new:"
                return "done"
            end
        ]])
        t:eq(true, ok, "apply ok(写)")
        mod.render(0)
        t:eq("new:1", mod.render2(1), "写入落到原 cell,未替换的 render2 也看得到")
        package.loaded.hotfix_deepup_mod = nil
    end

    -- ── 子段 20:值恰好是函数的"回调槽"仍须嫁接 ───────────────────────────
    -- local cb 是状态(由 M.set 在运行期赋值),不是 patch 自带的 helper。若判据把原模块侧
    -- 也算进去,cb 被赋过值之后 oval 就是函数 → 漏掉嫁接 → patch 绑到自己那份 nil 上,
    -- 首次调用 attempt to call a nil value,而 apply 照样返回 true。
    -- 且同一份 patch 的行为会取决于热修时 cb 有没有被赋过值,这本身就不可接受
    do
        package.loaded.hotfix_cb_mod = nil
        local src = [[
            local M = {}
            local cb
            function M.set(f) cb = f end
            function M.run(x) return cb(x) end
            return M
        ]]
        local m = assert(load(src, "=hotfix_cb_mod"))()
        package.loaded.hotfix_cb_mod = m
        m.set(function(x) return "cb:" .. x end)-- 关键:嫁接前先让 cb 持有函数
        t:eq("cb:1", m.run(1), "原 run 用已设置的 cb")
        t:eq(true, hotfix.apply("hotfix_cb_mod", [[
            local cb
            function M.run(x) return "v2/" .. cb(x) end
        ]]), "apply ok")
        t:eq("v2/cb:1", m.run(1), "patch 的 run 仍看到原 cb(未因值是函数而漏嫁接)")
        package.loaded.hotfix_cb_mod = nil
    end

    -- ── 子段 21:mod 表里混入别的 chunk 编译的函数 → 不拿它当扫描起点 ─────────
    do
        package.loaded.hotfix_bmod = nil
        package.loaded.hotfix_amod = nil
        package.loaded.hotfix_cmod = nil
        -- secret 只被非导出的 _inner 捕获:递归扫描够得着它,正因如此更要卡住模块边界
        local srcb = [[
            local M = {}
            local secret = "B-secret"
            local function _inner() return secret end
            function M.helper() return _inner() end
            function M.peek() return secret end
            return M
        ]]
        package.loaded.hotfix_bmod = assert(load(srcb, "=hotfix_bmod"))()
        package.loaded.hotfix_amod = assert(load(
            "local M = {} function M.own() return 1 end return M", "=hotfix_amod"))()
        -- A 把 B 的函数挂到自己表上(re-export);它的 source 是 B,不该成为 A 的扫描起点
        package.loaded.hotfix_amod.helper = package.loaded.hotfix_bmod.helper
        local ok = hotfix.apply("hotfix_amod",
            'secret = "OVERWRITTEN" function M.own() return 2 end')
        t:eq(true, ok, "A 的补丁照常应用")
        t:eq("B-secret", package.loaded.hotfix_bmod.peek(), "B 的 upvalue 未被 A 的补丁改写")
        t:eq(2, package.loaded.hotfix_amod.own(), "A 自己的函数已替换")
        _G.secret = nil-- 未命中 upmap 的裸写按设计落到 _G,清掉免得影响别的用例
        -- 整张表都是外来函数:upmap 必为空,与其静默把补丁的写全丢给 _G,不如明确失败
        package.loaded.hotfix_cmod = {helper = package.loaded.hotfix_bmod.helper}
        local ok2, err2 = hotfix.apply("hotfix_cmod", "function M.helper() return 0 end")
        t:eq(false, ok2, "认不出自家 chunk 时拒绝")
        t:check(err2 and nil ~= err2:find("locate module chunk"), "err 含 'locate module chunk'")
        package.loaded.hotfix_amod = nil
        package.loaded.hotfix_bmod = nil
        package.loaded.hotfix_cmod = nil
    end

    -- ── 子段 22:patch 的 helper 与被替换函数共用同一个 chunk local → 嫁接须递归进 helper ──
    -- 只走被替换函数自己的槽是不够的:M.bump 那份 counter 嫁接到了原模块 cell,而 helper 那份
    -- 还指着补丁自己那个 nil,同一个名字在补丁内部裂成两份、此后各写各的。症状是模块状态从此
    -- 不动(bump 恒返旧值),而 apply 返回 true、全程无报错无告警
    do
        local mod = _setup_module()
        mod.bump()
        mod.bump()-- counter=2
        local patch = [[
            local counter
            local function _tick()
                counter = (counter or 0) + 1000
            end
            function M.bump()
                _tick()
                return counter
            end
        ]]
        local ok = hotfix.apply("hotfix_unit_mod", patch)
        t:eq(true, ok, "apply ok")
        -- helper 与 M.bump 同一个 cell 且都嫁接到原模块那份:2 + 1000
        t:eq(1002, mod.bump(), "helper 的写入落到原 cell(裂成两份则恒为 2)")
        t:eq(2002, mod.bump(), "继续在同一 cell 上累加")
    end

    -- ── 子段 23:反复热修不逐代成链 ──────────────────────────────────────
    -- 每代 patch closure 只要引用过全局就持有本代 env(_ENV 不嫁接),env 的元方法持有替换前
    -- 扫出来的 upmap,而 upmap 的 entry.fn 正是上一代 closure,它又持上一代 env……于是历史代
    -- 一代都回收不掉。替换后把 upmap 重指到 mod 上现装着的那批即可断链
    do
        package.loaded.hotfix_gen_mod = nil
        local src = [[
            local M = {}
            local n = 0
            function M.step()
                n = n + 1
                return n
            end
            return M
        ]]
        local mod = assert(load(src, "=hotfix_gen_mod"))()
        package.loaded.hotfix_gen_mod = mod
        local wt = setmetatable({}, {__mode = "v"})
        local rounds = 20
        local allok = true
        -- 补丁同时碰全局(tostring → 持有 _ENV)与模块级 local(n → 让 upmap 记下本代 closure);
        -- 两环缺一就成不了链,也就测不出问题
        local patch = [[
            local n
            function M.step()
                local _ = tostring(1)
                n = (n or 0) + 1
                return n
            end
        ]]
        local i = 1
        while i <= rounds do
            if not hotfix.apply("hotfix_gen_mod", patch) then
                allok = false
            end
            wt[i] = mod.step
            i = i + 1
        end
        t:eq(true, allok, rounds .. " 代 apply 全部成功")
        collectgarbage()
        collectgarbage()
        collectgarbage()
        local alive = 0
        i = 1
        while i <= rounds do
            if wt[i] then
                alive = alive + 1
            end
            i = i + 1
        end
        -- 只该剩下当前装在 mod 上那一代;放宽到 3 是留 GC 时机余量,成链时这里等于 rounds
        t:check(alive <= 3, "历史代已回收,存活 " .. alive .. "/" .. rounds)
        package.loaded.hotfix_gen_mod = nil
    end

    -- ── 子段 24:拦不住的那种遮蔽风险要在 apply 的 detail 里点出来 ────────────
    -- 补丁在**函数体内**裸读遮蔽名,只有那次调用才会抛错;裸标识符编译成 _ENV.name,名字不在
    -- upvalue 列表里、常量表也取不到,apply 查不出来(子段 15 拦得住的是顶层裸读与路径 A)。
    -- 所以退一步:模块有遮蔽名 + 补丁确实读全局时,把该验哪几个名字写进 detail
    do
        package.loaded.hotfix_warn_mod = nil
        local src = [[
            local M = {}
            do local v = 1; function M.f1() return v end end
            do local v = 2; function M.f2() return v end end
            return M
        ]]
        local function _fresh()
            package.loaded.hotfix_warn_mod = assert(load(src, "=hotfix_warn_mod"))()
        end
        -- 读全局(tostring → 持有 _ENV)且模块有遮蔽名 → 带提示
        _fresh()
        local ok1, msg1 = hotfix.apply("hotfix_warn_mod", "function M.f1() return tostring(3) end")
        t:eq(true, ok1, "apply ok(读全局)")
        t:check(msg1 and nil ~= msg1:find("shadowed upvalue", 1, true),
            "detail 带遮蔽风险提示: " .. tostring(msg1))
        t:check(msg1 and nil ~= msg1:find("(s) v", 1, true), "提示里列出了遮蔽名 v")
        -- 补丁一个全局都不读 → 不可能走 path-B,不该加提示
        _fresh()
        local ok2, msg2 = hotfix.apply("hotfix_warn_mod", "function M.f1() return 3 end")
        t:eq(true, ok2, "apply ok(不读全局)")
        t:eq(nil, msg2:find("shadowed upvalue", 1, true), "补丁不读全局时不加提示")
        package.loaded.hotfix_warn_mod = nil
        -- 模块本身没有遮蔽名 → 即使补丁读全局也不该加提示
        _setup_module()
        local ok3, msg3 = hotfix.apply("hotfix_unit_mod", "function M.handle() return tostring(1) end")
        t:eq(true, ok3, "apply ok(无遮蔽名)")
        t:eq(nil, msg3:find("shadowed upvalue", 1, true), "模块无遮蔽名时不加提示")
    end
end)
end)
