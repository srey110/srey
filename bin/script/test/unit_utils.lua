-- utils 绑定层单元测试：hashring / trend / srey.utils (id/hex/ud_str/csprng_rand)
-- 外加一组跨模块的 (指针,长度) 入口负长度回归

local srey    = require("lib.srey")
local runner  = require("test.runner")
local utils   = require("srey.utils")
local hashring = require("srey.hashring")
local trend   = require("srey.trend")
local yyjson  = require("yyjson")
local seri    = require("srey.seri")
local dns     = require("srey.dns")
local websock = require("srey.websock")
local custz   = require("srey.custz")

srey.startup(function()
runner.run(function(t)
    -- ── srey.utils ─────────────────────────────────────────────────────
    do
        -- id() 同一线程内递增（跨线程不保证先后）
        local a = utils.id()
        local b = utils.id()
        t:check(type(a) == "number" and type(b) == "number", "id() returns number")
        t:check(b > a, "id() monotonic increasing")
    end
    do
        -- hex 编码字符串（tohex 输出大写）
        t:eq("616263", utils.hex("abc", true), "hex abc")
        t:eq("FF00",   utils.hex("\xff\x00"),    "hex binary (大写)")
        t:eq("ff00",   utils.hex("\xff\x00", true),  "hex binary 小写")
        t:eq("FF00",   utils.hex("\xff\x00", false), "显式 false 仍大写")
        -- 字符串形态下 lower 紧跟 data，多传一个 size 就把它顶掉；而 Lua 里任何数字都为真，
        -- 于是"缺省该大写"的调用会悄悄输出小写。卡死类型让它报错而不是给错结果
        t:eq(false, pcall(utils.hex, "\xff\x00", 2, false), "字符串多传 size 报错")
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
        utils.log_setlv(lv) -- 写回原值确保不破坏其他模块
        t:eq(lv, utils.log_getlv(), "log_setlv round-trip")
        -- 越界等级必须报错。log_setlv(-1) 会让 slog 的 lv > _log_lv 判定短路掉全进程日志
        t:eq(false, pcall(function() utils.log_setlv(-1) end), "log_setlv(-1) 报错")
        t:eq(false, pcall(function() utils.log_setlv(5) end), "log_setlv(5) 报错")
        t:eq(lv, utils.log_getlv(), "越界调用后等级不变")
    end
    do
        -- ud_str：nil 与 NULL light userdata(yyjson.null) 均优雅返回 nil，不解引用崩溃
        t:eq(nil, utils.ud_str(nil), "ud_str(nil) returns nil")
        t:eq(nil, utils.ud_str(yyjson.null, 5), "ud_str(NULL lud, size>0) returns nil")
        t:eq("", utils.ud_str(yyjson.null, 0), "ud_str(NULL lud, 0) returns empty")
    end

    -- ── hashring ───────────────────────────────────────────────────────
    do
        local ring = hashring.new()
        t:eq(true, ring:add(64, "node1"), "hashring add node1")
        t:eq(true, ring:add(64, "node2"), "hashring add node2")
        t:eq(true, ring:add(64, "node3"), "hashring add node3")
        -- 重复添加：当前实现下重复名称应失败
        t:eq(false, ring:add(64, "node1"), "hashring add dup")

        -- nreplicas 无上限时能让 C 层去要几十 GB，而 _realloc 分配失败是直接 exit 整个进程。
        -- 负数与超 uint32 由绑定层报错拒掉（先前靠 (uint32_t) 转换恰好落在上限之外才返 false，
        -- 换个数就能穿过去）；落在 uint32 内的超限值与 0 仍由 C 层返 false
        t:eq(true, not pcall(ring.add, ring, -1, "toobig"), "hashring add 负 nreplicas 报错")
        t:eq(true, not pcall(ring.add, ring, 4294967296, "toobig"), "hashring add 超 uint32 报错")
        t:eq(false, ring:add(1073741824, "toobig"), "hashring add 超上限 nreplicas 被拒")
        t:eq(false, ring:add(0, "zero"), "hashring add 零 nreplicas 被拒")
        -- 被拒的添加不得留下残节点：同名再按合法值添加须成功
        t:eq(true, ring:add(8, "toobig"), "被拒后同名合法添加仍成功")

        -- find 落点一致性（同 key 多次查询返回同一节点）
        local hit1 = ring:find("user:42")
        local hit2 = ring:find("user:42")
        t:check(hit1 ~= nil, "hashring find returns node")
        t:eq(hit1, hit2, "hashring find consistent")
        t:check(hit1 == "node1" or hit1 == "node2" or hit1 == "node3" or hit1 == "toobig",
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

    -- ── (指针,长度) 入口：长度必须挡住负数 ─────────────────────────────
    -- 负数转 size_t 是个天文数字，下游"剩余长度 < 需要长度"那类判定会全部恒假，
    -- 于是照着缓冲后面的堆内存一路读下去（hashring:find 更是直接喂进 xxh64 的裸读循环，
    -- 沿途没有任何分配失败或上限判断可以兜底）。这些入口现在统一走 lpub_check_lens /
    -- lpub_check_buf，一律报可被 pcall 捕获的 Lua 错。
    -- 这些都是收发缓冲，没有协议上界，故 max 传 0 只校验下界；BSON 那边传 INT32_MAX
    do
        local function rejects(fn, ...)
            return not pcall(fn, ...)
        end
        local ptr, size = seri.pack("probe")
        t:check(ptr ~= nil and size > 0, "取一块合法的 (指针,长度) 当探针")

        t:eq(true, rejects(seri.unpack, ptr, -1), "seri.unpack 负长度被拒")
        local _, err = pcall(seri.unpack, ptr, -1)
        t:check(type(err) == "string" and nil ~= err:find("out of range"),
                "通用入口与 BSON 入口共用同一句越界报错")
        t:eq(true, rejects(dns.unpack, ptr, -1, 0), "dns.unpack 负长度被拒")
        -- 唯一能写出界的那个：长度回绕后 MALLOC 只要到 13 字节，紧接着的 memcpy 却按
        -- SIZE_MAX 拷，从这个小堆块起一路覆写相邻内存
        t:eq(true, rejects(websock.pack_continua, 1, 0, ptr, -1), "websock.pack_continua 负长度被拒")
        -- 与 pack_text / pack_binary 对齐：lightuserdata 形态的长度是必填，不再默认按 0
        t:eq(true, rejects(websock.pack_continua, 1, 0, ptr), "websock.pack_continua 缺长度被拒")

        local ring = hashring.new()
        ring:add(8, "n1")
        t:eq(true, rejects(ring.find, ring, ptr, -1), "hashring:find 负长度被拒")
        t:eq(true, rejects(ring.add, ring, 8, ptr, -1), "hashring:add 负长度被拒")
        t:eq(true, rejects(ring.remove, ring, ptr, -1), "hashring:remove 负长度被拒")

        -- 端口入口：统一走 lpub_check_u16(PORT_OUT_OF_RANGE)，越界当场 argerror 而不是静默截断。
        -- 截断是语义反转不是数值偏差 —— 65536 截成 0，listen 照样返回合法监听 id，
        -- 内核却分配了随机临时端口，服务"起来了"但在预期端口上不可达
        t:eq(true, rejects(srey.listen, PACK_TYPE.HTTP, SSL_NAME.NONE, "127.0.0.1", 65536),
             "listen 端口 65536 被拒")
        t:eq(true, rejects(srey.listen, PACK_TYPE.HTTP, SSL_NAME.NONE, "127.0.0.1", -1),
             "listen 端口 -1 被拒")
        t:eq(true, rejects(srey.connect, PACK_TYPE.HTTP, SSL_NAME.NONE, "127.0.0.1", 65616),
             "connect 端口 65616 被拒")
        local _, perr = pcall(srey.listen, PACK_TYPE.HTTP, SSL_NAME.NONE, "127.0.0.1", 70000)
        t:check(type(perr) == "string" and nil ~= perr:find("port out of range"),
                "端口越界报的是 port out of range")

        -- pktype 入口：枚举外的值当场报错，不能一路存进 ud->pktype。
        -- 那样 prots.c 的 _prots_vtbl 会落到 _vtbl_none，收发"跑得起来"却只是原样透传，业务查不出所以然
        t:eq(true, rejects(srey.listen, 999, SSL_NAME.NONE, "127.0.0.1", 18999),
             "listen 未知 pktype 被拒")
        t:eq(true, rejects(srey.connect, 999, SSL_NAME.NONE, "127.0.0.1", 18999),
             "connect 未知 pktype 被拒")
        local _, kerr = pcall(srey.listen, 999, SSL_NAME.NONE, "127.0.0.1", 18999)
        t:check(type(kerr) == "string" and nil ~= kerr:find("unknown pack type"),
                "未知 pktype 报的是 unknown pack type")
        -- custz.pack 更严：非 PACK_CUSTZ_* 传进 custz_pack 会撞 ASSERTAB 直接 abort 进程
        t:eq(true, rejects(custz.pack, PACK_TYPE.HTTP, "x"),
             "custz.pack 非 custz 子类型被拒")
        t:eq(true, rejects(custz.pack, 999, "x"),
             "custz.pack 未知 pktype 被拒")
        -- sock_pack_type 是第五个 pktype 入口，且它落到的 ud->pktype 是 uint16_t，
        -- 不校验的话 65538 截成 2 就成了 PACK_HTTP，连接的解包协议被静默换掉
        t:eq(true, rejects(srey.sock_pack_type, 1, 1, 65538),
             "sock_pack_type 越界 pktype 被拒")
        t:eq(true, rejects(srey.sock_pack_type, 1, 1, 999),
             "sock_pack_type 未知 pktype 被拒")

        local cp, cl = custz.pack(PACK_TYPE.CUSTZ_FIXED, "x")
        t:check(cp ~= nil and cl > 1, "custz.pack 合法子类型仍可组包")
        utils.ud_free(cp)

        -- 合法长度照常工作，别把正常路径一起挡了
        t:eq("probe", (seri.unpack(ptr, size)), "合法长度不受影响")
        local frame, flens = websock.pack_continua(0, 1, ptr, size)
        t:check(frame ~= nil and flens > size, "pack_continua 合法长度仍可组帧")
        utils.ud_free(frame)
        utils.ud_free(ptr)
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
