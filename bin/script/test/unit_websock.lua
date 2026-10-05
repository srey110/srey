-- lib.websock 客户端两处修复:
--   1) scheme 大小写无关(RFC 3986 §3.1,C 侧 coro_utils 用 buf_icompare):"WS://" 必须能连上,
--      且默认端口不能因大小写落错分支;
--   2) text_continua 的生产者返回非 string / userdata 缺 size 是违约,须记 ERROR 并返回 false,
--      不可当成正常流结束——那会把截断的消息以 fin=1 收尾,对端当成一条完整消息。
-- 同一 task 内起 WEBSOCK 监听(C 层自动完成升级握手)再连回本机,server 侧只在第 3) 段原样回帧,其余不回。
-- 末尾另有 MQTT over WS 一段:连 test.server_ws(15003) 走完 bind + CONNECT/CONNACK。
-- 3) websock.frame(多返回值)与 websock.unpack(表)是两份平行实现:收到的每一帧两边逐字段比，
--    覆盖 0/125/126 长度、分片、ping/pong/close、mask 0/1，以及带子协议的帧。

local srey   = require("lib.srey")
local runner = require("test.runner")
local wbsk   = require("lib.websock")
local utils  = require("srey.utils")
local websock = require("srey.websock")

-- frame 与 unpack 逐字段比的计数与不一致记录
local ws_ncmp = 0
local ws_mis = {}

-- 组帧参数的截断回归：mask/fin 曾用裸 (int32_t) 转换，2^32 静默变 0 —— 掩码位被清掉的帧
-- 违反 RFC 6455 §5.1，对端必须断连；fin 被清掉则把终帧变成非终帧，卡死对端的分片重组
local function _test_frame_flag_range(t)
    t:eq(false, pcall(wbsk.text_fin, 4294967296, 1, "hi"), "pack_text: mask 2^32 被拒而非截成 0")
    t:eq(false, pcall(wbsk.text_fin, 1, 256, "hi"), "pack_text: fin 256 被拒而非截成 0")
    t:eq(false, pcall(wbsk.text_fin, 2, 1, "hi"), "pack_text: mask 2 被拒")
    t:eq(false, pcall(wbsk.ping, -1), "pack_ping: mask 负值被拒")
    t:eq(false, pcall(wbsk.continua, 1, 2, "hi"), "pack_continua: fin 2 被拒")
    -- 放行的这条会真的组出帧，返回的指针归调用方，丢掉就是泄漏
    local ok, pk = pcall(wbsk.text_fin, 0, 1, "hi")
    t:eq(true, ok, "pack_text: 合法 0/1 照常接受")
    if ok then utils.ud_free(pk) end
    -- netev 非整数在 core.connect 里抛出，此时 _handshake 已经把 hspack / hsctx 分配出来了，
    -- 两块都还没转移给 C 层。漏接的话每调一次泄漏两块，退出时的内存检查报出来
    t:eq(false, pcall(wbsk.connect, "ws://127.0.0.1:1/x", SSL_NAME.NONE, nil, false),
         "netev 非整数时 wbsk.connect 抛出，握手包与 hsctx 不留孤儿")
end

local PORT = 15048

-- 同一帧分别过 frame 与 unpack：返回值恒为 6 个、零长帧 data 也不为 nil、各字段与 unpack 的表一致
local function _ws_compare(pack, label)
    local nret = select("#", websock.frame(pack))
    local fin, prot, secprot, secpack, data, size = websock.frame(pack)
    local u = websock.unpack(pack)
    ws_ncmp = ws_ncmp + 1
    if 6 ~= nret or nil == data or fin ~= u.fin or prot ~= u.prot or secprot ~= u.secprot
        or secpack ~= u.secpack or data ~= u.data or size ~= u.size then
        ws_mis[#ws_mis + 1] = label
    end
    return fin, prot, data, size
end

-- 第 1 块正常(先发出首帧,让对端进入 continuation 累积状态),第 2 块返回 number 触发违约
local function _bad_producer(state)
    state.n = state.n + 1
    if 1 == state.n then
        return "aa"
    end
    return 42
end

srey.startup(function()
runner.run(function(t)
    -- 收到的每一帧都过一遍 _ws_compare。只有 frame 那段把 echo 打开:服务端不带掩码原样回一帧
    -- (close 不回)，客户端那头再比一次；其余用例只关心客户端 API 的返回值，server 侧不回
    local echo = false
    local srv_got, cli_got = {}, {}
    srey.on_recved(function(pktype, sk, client, slice, data)
        if PACK_TYPE.WEBSOCK ~= pktype then
            return
        end
        local fin, prot, d, sz = _ws_compare(data, (0 == client and "srv#" or "cli#") .. ws_ncmp)
        if not echo then
            return
        end
        local rec = { fin = fin, prot = prot, slice = slice, body = srey.ud_str(d, sz) or "" }
        if 0 ~= client then
            cli_got[#cli_got + 1] = rec
            return
        end
        srv_got[#srv_got + 1] = rec
        local f, fs
        if WEBSOCK_PROT.PING == prot then
            f, fs = websock.pack_ping(0)
        elseif WEBSOCK_PROT.PONG == prot then
            f, fs = websock.pack_pong(0)
        elseif WEBSOCK_PROT.CONTINUA == prot then
            f, fs = websock.pack_continua(0, fin, rec.body)
        elseif WEBSOCK_PROT.TEXT == prot then
            f, fs = websock.pack_text(0, fin, rec.body)
        elseif WEBSOCK_PROT.BINARY == prot then
            f, fs = websock.pack_binary(0, fin, rec.body)
        end
        if f then
            srey.send(sk, f, fs, 0)
        end
    end)

    local lid = srey.listen(PACK_TYPE.WEBSOCK, SSL_NAME.NONE, "0.0.0.0", PORT)
    t:check(ERR_FAILED ~= lid, "listen " .. PORT)
    if ERR_FAILED == lid then
        return
    end

    -- 大写 scheme:修复前 _parse_url 的 "ws" ~= url.scheme 会直接拒掉,连接这步就拿不到 fd
    local sk = wbsk.connect("WS://127.0.0.1:" .. PORT .. "/", SSL_NAME.NONE)
    t:check(sk and sk.valid, "大写 scheme 的 ws URL 可连接")
    if sk and sk.valid then
        t:eq(false, wbsk.text_continua(sk, 1, _bad_producer, { n = 0 }),
             "生产者违约 text_continua 返回 false")
        srey.close(sk)
    end

    -- 协商结果要落到名字上，不能只判 spctx 非 nil：回显成另一个子协议，
    -- 应用层照着它选编解码就全错。secprots 返回 (匹配下标 0 起, 全部名字 1 起)，
    -- 两者一起断言顺带钉住那个 0→1 的下标换算
    local sky, spy = wbsk.connect("ws://127.0.0.1:" .. PORT .. "/", SSL_NAME.NONE, "mqtt")
    local yidx, yprots = websock.secprots(spy)
    t:eq("mqtt", yidx and yprots and yidx >= 0 and yprots[yidx + 1] or nil,
         "服务端支持的子协议(mqtt)须协商成功并原样回显")
    if sky and sky.valid then
        srey.close(sky)
    end

    -- 非内建子协议走透传：服务端回显客户端提的第一个，sectype 保持 PACK_NONE，
    -- 由应用层自己实现该子协议（C 侧 task_timeout.c 的 chat 用例是同一条路径）
    local skn, spn = wbsk.connect("ws://127.0.0.1:" .. PORT .. "/", SSL_NAME.NONE, "chat")
    t:check(skn and skn.valid, "非内建子协议可握手")
    local nidx, nprots = websock.secprots(spn)
    t:eq("chat", nidx and nprots and nidx >= 0 and nprots[nidx + 1] or nil,
         "非内建子协议(chat)按透传原样回显")
    if skn and skn.valid then
        srey.close(skn)
    end

    -- ── 绑定层的返回值个数必须恒定 ──────────────────────────────────────
    -- 标注里 secprots 的第二个返回值是 string[]、pack_handshake 是三个，业务照标注
    -- 按位置取值。失败时少返几个的话，多赋值会静默补 nil（还能撑住），但直接把返回值
    -- 塞进另一个调用（srey.send(sk, websock.pack_handshake(...))）就整体错位了
    do
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
        -- 成功路径不在这里验：上面 wbsk.connect 的用例走的就是它，unit_protocol.lua 的
        -- websock pack 系列还单测了"不连接、直接 ud_free(hsctx)"这条——所有权只在传给
        -- srey.connect 那一刻才转交框架，没走到 connect 时它归调用方，释放途径是有的
    end

    -- ── frame 与 unpack 逐字段一致 ──────────────────────────────────────
    -- 客户端发 mask=1 的帧、服务端回 mask=0 的帧，两头收到的都比一次
    local wsk = wbsk.connect("ws://127.0.0.1:" .. PORT .. "/", SSL_NAME.NONE)
    if t:check(wsk and wsk.valid, "frame 用例连上") then
        echo = true
        -- { 组帧函数, fin, 载荷, 期望的 slice }；控制帧没有载荷参数
        local frames = {}
        for _, n in ipairs({ 0, 125, 126 }) do
            frames[#frames + 1] = { websock.pack_text, 1, string.rep("t", n), 0 }
            frames[#frames + 1] = { websock.pack_binary, 1, string.rep("b", n), 0 }
        end
        frames[#frames + 1] = { websock.pack_text, 0, "aa", SLICE_TYPE.START }
        frames[#frames + 1] = { websock.pack_continua, 0, "", SLICE_TYPE.SLICE }
        frames[#frames + 1] = { websock.pack_continua, 0, string.rep("c", 126), SLICE_TYPE.SLICE }
        frames[#frames + 1] = { websock.pack_continua, 1, "zz", SLICE_TYPE.END }
        frames[#frames + 1] = { websock.pack_ping }
        frames[#frames + 1] = { websock.pack_pong }
        frames[#frames + 1] = { websock.pack_close }
        local function wait(nsrv, ncli)
            for _ = 1, 150 do
                if #srv_got >= nsrv and #cli_got >= ncli then
                    break
                end
                srey.sleep(20)
            end
        end
        local f, fs
        for _, e in ipairs(frames) do
            if e[2] then
                f, fs = e[1](1, e[2], e[3])
            else
                f, fs = e[1](1)
            end
            -- close 等前面的回帧都到了再发：服务端收到 close 就断连，排在后面的回帧发不出去
            if e[1] == websock.pack_close then
                wait(#frames - 1, #frames - 1)
            end
            srey.send(wsk, f, fs, 0)
        end
        wait(#frames, #frames - 1)
        echo = false
        t:eq(#frames, #srv_got, "服务端收齐全部帧")
        t:eq(#frames - 1, #cli_got, "客户端收齐回帧(close 不回)")
        for i, e in ipairs(frames) do
            if e[2] then
                t:eq(e[3], srv_got[i] and srv_got[i].body, "服务端载荷 #" .. i)
                t:eq(e[4], srv_got[i] and srv_got[i].slice, "服务端 slice #" .. i)
                t:eq(e[3], cli_got[i] and cli_got[i].body, "客户端载荷 #" .. i)
            end
        end
        t:eq(WEBSOCK_PROT.PING, srv_got[#frames - 2] and srv_got[#frames - 2].prot, "ping 帧")
        t:eq(WEBSOCK_PROT.PONG, srv_got[#frames - 1] and srv_got[#frames - 1].prot, "pong 帧")
        t:eq(WEBSOCK_PROT.CLOSE, srv_got[#frames] and srv_got[#frames].prot, "close 帧")
        srey.close(wsk)
    end
    t:check(ws_ncmp >= 25, "frame 与 unpack 比过的帧数 " .. ws_ncmp)
    t:eq(0, #ws_mis, "frame 与 unpack 逐字段一致: " .. table.concat(ws_mis, ","))

    srey.unlisten(lid)-- 释放端口给后续测试
    _test_frame_flag_range(t)

    -- ── MQTT over WS：连 test.server_ws(15003) 的 mqtt 分支 ──────────────
    -- C 侧同一条路径是 test/task_timeout.c 的 _timeout_ws 配 test/task_ws_server.c；这里只覆盖
    -- Lua 侧：mqtt.ws_bind 绑定层、pack_connect 组包、wbsk.unpack 取 secprot / secpack，
    -- 以及 test/server_ws.lua 那个 mqtt 分支(在此之前全程不执行)。
    -- server_ws 在 test.test 里注册在 TESTS 之后,起来的顺序不定,故先等 500ms(同 unit_lib)
    do
        local cmqtt = require("srey.mqtt")
        local V311, CONNACK = 4, 0x02-- mqtt_protversion / mqtt_prot,Lua 侧无对应常量表
        srey.sleep(500)
        local skm, spm = wbsk.connect("ws://127.0.0.1:15003/", SSL_NAME.NONE, "mqtt")
        t:check(skm and skm.valid, "连上 server_ws 的 mqtt 子协议")
        if skm and skm.valid then
            local midx, mprots = websock.secprots(spm)
            t:eq("mqtt", midx and mprots and midx >= 0 and mprots[midx + 1] or nil,
                 "mqtt 子协议协商成功")
            -- ws_bind 必须由协商结果门控:没协商到 mqtt 时 ws->ud 为 NULL,注入会被判掉
            -- 并就地断连,症状变成后续 syn_send 莫名失败而不是"没协商上"
            if spm then
                t:eq(true, cmqtt.ws_bind(skm, V311), "mqtt.ws_bind 投递成功")
                local conn, clens = cmqtt.pack_connect(V311, 1, 60, "luawsmqtt")
                t:check(conn ~= nil, "mqtt.pack_connect 组包成功")
                if conn then
                    -- client=1:客户端帧必须带掩码。帧内已复制 conn,组完即可释放它;
                    -- 帧本身 copy=0 转交框架,不再 ud_free
                    local frame, fsize = wbsk.binary_fin(1, 1, conn, clens)
                    utils.ud_free(conn)
                    t:check(frame ~= nil, "binary_fin 组帧成功")
                    if frame then
                        local rdata = srey.syn_send(skm, frame, fsize, 0)
                        t:check(rdata ~= nil, "收到 server_ws 的响应")
                        if rdata then
                            -- 带子协议的帧：frame 的 secprot / secpack 走非 nil 那一支
                            local nmis = #ws_mis
                            _ws_compare(rdata, "mqtt")
                            t:eq(nmis, #ws_mis, "带子协议的帧 frame 与 unpack 一致")
                            t:eq(PACK_TYPE.MQTT, select(3, wbsk.frame(rdata)), "frame 的 secprot 为 MQTT")
                            local pack = wbsk.unpack(rdata)
                            t:eq(PACK_TYPE.MQTT, pack and pack.secprot, "响应帧 secprot 为 MQTT")
                            t:check(pack and pack.secpack ~= nil, "响应帧带 secpack")
                            if pack and pack.secpack then
                                t:eq(CONNACK, cmqtt.prot(pack.secpack), "服务端回的是 CONNACK")
                            end
                        end
                    end
                end
            end
            srey.close(skm)
        end
    end
end)
end)
