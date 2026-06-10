-- utils 绑定层单元测试：hashring / trend / srey.utils (id/hex/ud_str/csprng_rand)

local srey    = require("lib.srey")
local runner  = require("test.runner")
local utils   = require("srey.utils")
local hashring = require("srey.hashring")
local trend   = require("srey.trend")

srey.startup(function()
runner.run("utils", function(t)
    -- ── srey.utils ─────────────────────────────────────────────────────
    do
        -- id() 单调递增
        local a = utils.id()
        local b = utils.id()
        t:check(type(a) == "number" and type(b) == "number", "id() returns number")
        t:check(b > a, "id() monotonic increasing")
    end
    do
        -- hex 编码字符串（tohex 输出大写）
        t:eq("616263", utils.hex("abc"):lower(), "hex abc")
        t:eq("FF00",   utils.hex("\xff\x00"),    "hex binary (大写)")
    end
    do
        -- csprng_rand 返回指定长度且非空
        local r = utils.csprng_rand(32)
        t:check(r ~= nil and #r == 32, "csprng_rand 32 bytes")
        -- 两次结果不同
        local r2 = utils.csprng_rand(32)
        t:check(r ~= r2, "csprng_rand random")
    end
    do
        -- log_getlv / log_setlv 一致
        local lv = utils.log_getlv()
        t:check(type(lv) == "number", "log_getlv returns number")
        utils.log_setlv(lv)  -- 写回原值确保不破坏其他模块
        t:eq(lv, utils.log_getlv(), "log_setlv round-trip")
    end

    -- ── hashring ───────────────────────────────────────────────────────
    do
        local ring = hashring.new()
        t:eq(true, ring:add(64, "node1"), "hashring add node1")
        t:eq(true, ring:add(64, "node2"), "hashring add node2")
        t:eq(true, ring:add(64, "node3"), "hashring add node3")
        -- 重复添加：当前实现下重复名称应失败
        t:eq(false, ring:add(64, "node1"), "hashring add dup")

        -- find 落点一致性（同 key 多次查询返回同一节点）
        local hit1 = ring:find("user:42")
        local hit2 = ring:find("user:42")
        t:check(hit1 ~= nil, "hashring find returns node")
        t:eq(hit1, hit2, "hashring find consistent")
        t:check(hit1 == "node1" or hit1 == "node2" or hit1 == "node3",
                "hashring hit is one of nodes")

        -- remove 后落点应仅在剩余节点
        ring:remove(hit1)
        local hit3 = ring:find("user:42")
        t:check(hit3 ~= nil and hit3 ~= hit1, "hashring find after remove")

        -- 空环 find 返回 nil
        local empty = hashring.new()
        t:eq(nil, empty:find("anything"), "empty ring find nil")
    end

    -- ── trend ──────────────────────────────────────────────────────────
    do
        local tr = trend.new()
        -- 首次采样不忙
        t:eq(false, tr:busy(10, 4, 5), "trend first sample not busy")
        -- 持平不忙
        t:eq(false, tr:busy(10, 4, 5), "trend flat not busy")
        -- 上升不忙
        t:eq(false, tr:busy(20, 4, 5), "trend rising not busy")
        -- 跌幅 > 20% 视为忙（20 → 10，跌 50%）
        t:eq(true, tr:busy(10, 4, 5), "trend drop >20% busy")
    end

    -- ── lib/utils.lua 纯 lua 函数（实际依赖 srey.utils.csprng_rand 等）
    do
        -- randstr 长度正确，且字符在期望集内
        local s = randstr(16)
        t:check(s ~= nil and #s == 16, "randstr 16 length")
        t:check(s:match("^[0-9a-zA-Z]+$") ~= nil, "randstr charset")
        -- 两次结果不同
        t:check(randstr(16) ~= randstr(16), "randstr random")
    end
end)
end)
