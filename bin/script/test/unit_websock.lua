-- lib.websock 客户端两处修复:
--   1) scheme 大小写无关(RFC 3986 §3.1,C 侧 coro_utils 用 buf_icompare):"WS://" 必须能连上,
--      且默认端口不能因大小写落错分支;
--   2) text_continua 的生产者返回非 string / userdata 缺 size 是违约,须记 ERROR 并返回 false,
--      不可当成正常流结束——那会把截断的消息以 fin=1 收尾,对端当成一条完整消息。
-- 同一 task 内起 WEBSOCK 监听(C 层自动完成升级握手)再连回本机,server 侧不回任何数据。

local srey   = require("lib.srey")
local runner = require("test.runner")
local wbsk   = require("lib.websock")

-- 组帧参数的截断回归：mask/fin 曾用裸 (int32_t) 转换，2^32 静默变 0 —— 掩码位被清掉的帧
-- 违反 RFC 6455 §5.1，对端必须断连；fin 被清掉则把终帧变成非终帧，卡死对端的分片重组
local function _test_frame_flag_range(t)
    t:eq(false, pcall(wbsk.text_fin, 4294967296, 1, "hi"), "pack_text: mask 2^32 被拒而非截成 0")
    t:eq(false, pcall(wbsk.text_fin, 1, 256, "hi"), "pack_text: fin 256 被拒而非截成 0")
    t:eq(false, pcall(wbsk.text_fin, 2, 1, "hi"), "pack_text: mask 2 被拒")
    t:eq(false, pcall(wbsk.ping, -1), "pack_ping: mask 负值被拒")
    t:eq(false, pcall(wbsk.continua, 1, 2, "hi"), "pack_continua: fin 2 被拒")
    t:eq(true, pcall(wbsk.text_fin, 0, 1, "hi"), "pack_text: 合法 0/1 照常接受")
end

local PORT = 15048

-- 第 1 块正常(先发出首帧,让对端进入 continuation 累积状态),第 2 块返回 number 触发违约
local function _bad_producer(state)
    state.n = state.n + 1
    if 1 == state.n then
        return "aa"
    end
    return 42
end

srey.startup(function()
runner.run("websock_client", function(t)
    srey.on_recved(function()
        -- server 侧收到什么都不回:本用例只关心客户端 API 的返回值,不需要响应
    end)

    local lid = srey.listen(PACK_TYPE.WEBSOCK, SSL_NAME.NONE, "0.0.0.0", PORT)
    t:check(ERR_FAILED ~= lid, "listen " .. PORT)
    if ERR_FAILED == lid then
        return
    end

    -- 大写 scheme:修复前 _parse_url 的 "ws" ~= url.scheme 会直接拒掉,连接这步就拿不到 fd
    local fd, skid = wbsk.connect("WS://127.0.0.1:" .. PORT .. "/", SSL_NAME.NONE)
    t:check(fd and INVALID_SOCK ~= fd, "大写 scheme 的 ws URL 可连接")
    if fd and INVALID_SOCK ~= fd then
        t:eq(false, wbsk.text_continua(fd, skid, 1, _bad_producer, { n = 0 }),
             "生产者违约 text_continua 返回 false")
        srey.close(fd, skid)
    end

    local fdy, skidy, spy = wbsk.connect("ws://127.0.0.1:" .. PORT .. "/", SSL_NAME.NONE, "mqtt")
    t:check(spy ~= nil, "服务端支持的子协议(mqtt)须协商成功并回显")
    if fdy and INVALID_SOCK ~= fdy then
        srey.close(fdy, skidy)
    end

    -- 非内建子协议走透传：服务端回显客户端提的第一个，sectype 保持 PACK_NONE，
    -- 由应用层自己实现该子协议（C 侧 task_timeout.c 的 chat 用例是同一条路径）
    local fdn, skidn, spn = wbsk.connect("ws://127.0.0.1:" .. PORT .. "/", SSL_NAME.NONE, "chat")
    t:check(fdn and INVALID_SOCK ~= fdn, "非内建子协议可握手")
    t:check(spn ~= nil, "非内建子协议(chat)按透传回显")
    if fdn and INVALID_SOCK ~= fdn then
        srey.close(fdn, skidn)
    end

    -- ── 绑定层的返回值个数必须恒定 ──────────────────────────────────────
    -- 标注里 secprots 的第二个返回值是 string[]、pack_handshake 是三个，业务照标注
    -- 按位置取值。失败时少返几个的话，多赋值会静默补 nil（还能撑住），但直接把返回值
    -- 塞进另一个调用（srey.send(fd, skid, websock.pack_handshake(...))）就整体错位了
    do
        local websock = require("srey.websock")
        local idx, prots = websock.secprots(nil)
        t:eq(nil, idx,   "secprots(nil) 第一个返回值为 nil")
        t:eq(nil, prots, "secprots(nil) 第二个返回值也是 nil，个数仍为 2")
        t:eq(2, select("#", websock.secprots(nil)), "secprots 失败时返回值个数为 2")

        -- 子协议名不是 RFC 7230 token（括号是分隔符）→ websock_pack_handshake 返 NULL
        local badprot = "bad(proto)"
        t:eq(3, select("#", websock.pack_handshake("h", "/", badprot)),
             "pack_handshake 失败时返回值个数为 3")
        local hp, hs, hc = websock.pack_handshake("h", "/", badprot)
        t:check(hp == nil and hs == nil and hc == nil, "pack_handshake 失败时三个返回值均为 nil")
        -- 成功路径不在这里验：hsctx 的所有权只能由 srey.connect 接走，不连的话没有合法的
        -- 释放途径，测下来就是一处必然泄漏。上面 wbsk.connect 的用例已覆盖成功路径
    end

    srey.unlisten(lid)-- 释放端口给后续测试
    _test_frame_flag_range(t)
end)
end)
