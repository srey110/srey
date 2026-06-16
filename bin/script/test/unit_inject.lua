-- inject 单元测试:print 捕获 / _U 暴露 / _G 读透传 / 沙箱写隔离 / 字节码拒绝 / 编译错 / 运行错

local srey   = require("lib.srey")
local inject = require("lib.inject")
local runner = require("test.runner")

srey.startup(function()
runner.run("inject", function(t)
    -- ── 子段 1:print 捕获 → output 行 ──────────────────────────────
    do
        local ok, out = inject("print('hello')")
        t:eq(true, ok, "inject print ok")
        t:eq("hello", out and out[1], "捕获 print 输出 'hello'")
    end

    -- ── 子段 2:print 多参 tab 分隔 + 多行 ──────────────────────────
    do
        local ok, out = inject("print('a', 1, true)\nprint('b')")
        t:eq(true, ok, "inject 多 print ok")
        t:eq("a\t1\ttrue", out and out[1], "多参 tab 分隔")
        t:eq("b", out and out[2], "第二行独立")
    end

    -- ── 子段 3:_U 是 table(从 message_dispatch upvalue 树收集) ──────
    do
        local ok, out = inject("print(type(_U))")
        t:eq(true, ok, "inject 读 _U ok")
        t:eq("table", out and out[1], "_U 始终是 table")
    end

    -- ── 子段 4:_G 读透传(env __index = _G) ────────────────────────
    do
        local ok, out = inject("print(type(string))")
        t:eq(true, ok, "inject 读 _G ok")
        t:eq("table", out and out[1], "经 __index 读到 _G.string")
    end

    -- ── 子段 5:写新全局落 env 表,不污染 _G(沙箱隔离) ──────────────
    do
        local ok = inject("INJECT_TEST_LEAK = 1")
        t:eq(true, ok, "inject 写全局 ok")
        t:eq(nil, _G.INJECT_TEST_LEAK, "新全局落 env(无 __newindex),不污染 _G")
    end

    -- ── 子段 6:预编译字节码 → load 't' 模式拒绝(不可注入字节码) ─────
    do
        local ok, out = inject(string.dump(function() return 1 end))
        t:eq(false, ok, "字节码注入被拒绝")
        t:check(out and out[1] and nil ~= tostring(out[1]):find("binary"), "err 含 'binary'")
    end

    -- ── 子段 7:编译错 → false + err ────────────────────────────────
    do
        local ok, out = inject("this is not valid lua (((")
        t:eq(false, ok, "语法错返回 false")
        t:check(out and nil ~= out[1], "返回 err 信息")
    end

    -- ── 子段 8:运行错 → false + 末行含错误信息 ─────────────────────
    do
        local ok, out = inject("error('boom')")
        t:eq(false, ok, "运行错返回 false")
        t:check(out and #out >= 1 and nil ~= tostring(out[#out]):find("boom"), "末行含 'boom'")
    end

    -- ── 子段 9:print 含中间/尾随/单 nil → 按真实参数个数输出,不被 ipairs 截断 ──
    do
        local ok, out = inject("print('a', nil, 'c')")
        t:eq(true, ok, "inject 中间 nil print ok")
        t:eq("a\tnil\tc", out and out[1], "中间 nil 不截断,输出 'nil'")
    end
    do
        local ok, out = inject("print('x', nil)")
        t:eq(true, ok, "inject 尾随 nil print ok")
        t:eq("x\tnil", out and out[1], "尾随 nil 不丢失")
    end
    do
        local ok, out = inject("print(nil)")
        t:eq(true, ok, "inject 单 nil print ok")
        t:eq("nil", out and out[1], "单 nil 输出 'nil'")
    end
end)
end)
