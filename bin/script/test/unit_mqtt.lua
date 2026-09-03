-- mqtt 绑定层单元测试：pack_* 系列 wire format 首字节验证 + props/topics 写入

local srey   = require("lib.srey")
local runner = require("test.runner")
local utils  = require("srey.utils")
local mqtt   = require("lib.mqtt")
local mqttc  = require("srey.mqtt")-- 绑定层原始接口，测版本校验用
local yyjson = require("yyjson")-- yyjson.null 是一个 NULL lightuserdata，用来测空指针拒收

-- 取 pack 返回数据的首字节高 4 位（MQTT 控制类型）
local function _ptype(pack, size)
    if not pack or 0 == size then return -1 end
    local b = string.byte(srey.ud_str(pack, 1))
    return (b >> 4) & 0x0F
end

srey.startup(function()
runner.run(function(t)
    -- ── props 共享元表受保护 ───────────────────────────────────────────
    -- 元表是全类型共享的，getmetatable 若能拿到真表，业务一行 __gc = nil
    -- 就能让此后每个 props 都不再释放内部 binary_ctx
    do
        local p = mqtt.props()
        t:eq("_mqtt_props_ctx", getmetatable(p), "props 元表被 __metatable 挡住")
        t:eq(false, pcall(function() setmetatable(p, {}) end), "props 不可被换元表")
    end

    -- ── props 写入 ─────────────────────────────────────────────────────
    do
        local p = mqtt.props()
        p:fixnum(mqtt.PROP.SESSION_EXPIRY, 120)
        p:fixnum(mqtt.PROP.RECEIVE_MAXIMUM, 15000)
        p:kv(mqtt.PROP.USER_PROPERTY, "key1", "val1")
        p:varnum(mqtt.PROP.SUBSCRIPTION_ID, 99)
        -- 标识先收窄成 4 字节枚举再判的话,2^32+1 会截成 PAYLOAD_FORMAT 被静默收下
        t:eq(false, pcall(function() p:fixnum(0x100000001, 1) end), "属性标识 2^32+1 被拒")
        t:eq(false, pcall(function() p:varnum(0x2B, 1) end), "属性标识超 0x2A 被拒")
        t:eq(false, pcall(function() p:fixnum(0, 1) end), "属性标识 0 被拒")
        p:binary(mqtt.PROP.AUTH_DATA, "\x01\x02\x03")
        local data, size = p:data()
        t:check(data ~= nil and size > 0, "props data after writes")
        -- reset 后清空
        p:reset()
        data, size = p:data()
        t:check(data == nil and size == 0, "props after reset empty")
        p:free()
    end
    do
        -- topics 用于 SUBSCRIBE/UNSUBSCRIBE
        local topics = mqtt.props()
        topics:subscribe(mqtt.VERSION.V311, "/test/topic1", 1, 0, 0, 0)
        topics:subscribe(mqtt.VERSION.V50,  "/test/topic2", 2, 1, 0, 1)
        local data, size = topics:data()
        t:check(data ~= nil and size > 0, "topics subscribe wire")
        topics:reset()
        topics:unsubscribe("/test/topic1")
        data, size = topics:data()
        t:check(data ~= nil and size > 0, "topics unsubscribe wire")
        topics:free()
    end

    -- ── pack_connect / connack ─────────────────────────────────────────
    do
        -- v3.1.1 无 props
        local pack, size = mqtt.pack_connect(
            mqtt.VERSION.V311, 1, 60, "client-id-1",
            "user", "psw", nil, nil, 0, 0, nil, nil)
        t:check(pack ~= nil and size > 0, "pack_connect v311")
        t:eq(mqtt.PROT.CONNECT, _ptype(pack, size), "pack_connect type byte")
        utils.ud_free(pack)
    end
    do
        -- v5.0 with properties
        local cp = mqtt.props()
        cp:fixnum(mqtt.PROP.SESSION_EXPIRY, 120)
        cp:kv(mqtt.PROP.USER_PROPERTY, "k", "v")
        local pack, size = mqtt.pack_connect(
            mqtt.VERSION.V50, 1, 120, "client-id-2",
            "user", "psw", nil, nil, 0, 0, cp, nil)
        cp:free()
        t:check(pack ~= nil and size > 0, "pack_connect v50 with props")
        t:eq(mqtt.PROT.CONNECT, _ptype(pack, size), "pack_connect v50 type byte")
        utils.ud_free(pack)
    end
    do
        -- 组包失败（clientid 超 UINT16_MAX，mqtt_pack_connect 返 NULL）时，lpub_rtn_lud
        -- 压 2 个 nil 而不是 1 个：返回值个数与成功路径一致，业务把它整段塞进
        -- srey.send(fd, skid, mqtt.pack_connect(...)) 才不会错位
        local pack, size = mqtt.pack_connect(
            mqtt.VERSION.V311, 1, 60, string.rep("a", 70000),
            "user", "psw", nil, nil, 0, 0, nil, nil)
        t:eq(nil, pack, "pack_connect clientid 超长返 nil")
        t:eq(nil, size, "pack_connect 失败时第二个返回值也是 nil")
        t:eq(2, select("#", mqtt.pack_connect(
            mqtt.VERSION.V311, 1, 60, string.rep("a", 70000),
            "user", "psw", nil, nil, 0, 0, nil, nil)),
            "pack_connect 失败时返回值个数为 2")
    end
    do
        local pack, size = mqtt.pack_connack(mqtt.VERSION.V311, 1, 0, nil)
        t:check(pack ~= nil and size > 0, "pack_connack v311")
        t:eq(mqtt.PROT.CONNACK, _ptype(pack, size), "pack_connack type byte")
        utils.ud_free(pack)
    end

    -- ── 16 位 wire 字段越界必须报错，不能静默截断 ──────────────────────
    do
        -- 65536 截成 0，而 0 是 QoS>0 PUBLISH / SUBSCRIBE / UNSUBSCRIBE 禁用的保留 packid
        t:eq(false, pcall(mqtt.pack_publish, mqtt.VERSION.V311, 0, 1, 0, "/t", 65536, "x"),
             "pack_publish packid 65536 报错")
        t:eq(false, pcall(mqtt.pack_publish, mqtt.VERSION.V311, 0, 1, 0, "/t", -1, "x"),
             "pack_publish packid -1 报错")
        t:eq(false, pcall(mqtt.pack_puback, mqtt.VERSION.V311, 65536),
             "pack_puback packid 65536 报错")
        -- payload 走 _lmqtt_get_payload 自己那套 (指针, 长度) 读法，不经 lpub_check_buf，
        -- 空指针判定要单独加：剩余长度按 8 算却只写 0 字节，对端会吃掉下一条报文的头 8 字节
        t:eq(false, pcall(mqtt.pack_publish, mqtt.VERSION.V311, 0, 1, 0, "/t", 1, yyjson.null, 8),
             "pack_publish payload 空指针被拒")
        local okempty, epack = pcall(mqtt.pack_publish, mqtt.VERSION.V311, 0, 1, 0, "/t", 1, yyjson.null, 0)
        t:eq(true, okempty, "pack_publish 空指针 + 0 长度放行")
        if okempty and epack then
            utils.ud_free(epack)
        end
        -- topics 必须是真的 props 缓冲：传别的类型也会报错，那样测到的是类型校验不是 packid
        local subtopics = mqtt.props()
        subtopics:subscribe(mqtt.VERSION.V311, "/t", 0, 0, 0, 0)
        t:eq(false, pcall(mqtt.pack_subscribe, mqtt.VERSION.V311, 65536, subtopics, nil),
             "pack_subscribe packid 65536 报错")
        local oksub, packsub = pcall(mqtt.pack_subscribe, mqtt.VERSION.V311, 7, subtopics, nil)
        t:check(oksub and packsub ~= nil, "pack_subscribe 合法 packid 仍可用")
        if oksub and packsub then
            utils.ud_free(packsub)
        end
        subtopics:free()
        -- 65535 是合法上界，不能误伤
        local okmax, packmax = pcall(mqtt.pack_puback, mqtt.VERSION.V311, 65535)
        t:check(okmax and packmax ~= nil, "pack_puback packid 65535 合法")
        if okmax and packmax then
            utils.ud_free(packmax)
        end
        -- keepalive 同为 16 位：65536 截成 0 在 MQTT 里是"关掉保活"
        t:eq(false, pcall(mqtt.pack_connect, mqtt.VERSION.V311, 1, 65536, "cid"),
             "pack_connect keepalive 65536 报错")
    end

    -- ── pack_publish / puback / pubrec / pubrel / pubcomp ──────────────
    do
        for _, qos in ipairs({0, 1, 2}) do
            local pack, size = mqtt.pack_publish(
                mqtt.VERSION.V311, 0, qos, 0, "/topic", 100 + qos, "hello payload")
            t:check(pack ~= nil and size > 0, "pack_publish qos=" .. qos)
            t:eq(mqtt.PROT.PUBLISH, _ptype(pack, size), "pack_publish type byte qos=" .. qos)
            local txt = srey.ud_str(pack, size)
            t:check(txt:find("/topic", 1, true) ~= nil, "pack_publish has topic qos=" .. qos)
            t:check(txt:find("hello payload", 1, true) ~= nil, "pack_publish has payload qos=" .. qos)
            utils.ud_free(pack)
        end
    end
    do
        local pp = mqtt.props()
        pp:kv(mqtt.PROP.USER_PROPERTY, "pk", "pv")
        local p0, s0 = mqtt.pack_publish(mqtt.VERSION.V50, 0, 1, 0, "/t", 1, "hello")
        local p1, s1 = mqtt.pack_publish(mqtt.VERSION.V50, 0, 1, 0, "/t", 1, "hello", nil, pp)
        t:check(p0 ~= nil and p1 ~= nil, "pack_publish v50 两次组包均成功")
        t:check(s1 > s0, "string payload 按 9 位签名传 props 须生效(修复前槽位浮动到 8,props 静默丢弃)")
        t:check(srey.ud_str(p1, s1):find("pv", 1, true) ~= nil, "props 内容须出现在 PUBLISH 属性块")
        local slot8 = pcall(mqtt.pack_publish, mqtt.VERSION.V50, 0, 1, 0, "/t", 1, "hello", pp)
        t:eq(false, slot8, "字符串负载时 props 误放槽 8 须报错,而非静默丢弃属性块")
        utils.ud_free(p0)
        utils.ud_free(p1)
        local lud, ludsz = mqtt.pack_publish(mqtt.VERSION.V311, 0, 0, 0, "/x", 0, "payloadbytes")
        local p2, s2 = mqtt.pack_publish(mqtt.VERSION.V50, 0, 1, 0, "/t", 1, lud, ludsz)
        local p3, s3 = mqtt.pack_publish(mqtt.VERSION.V50, 0, 1, 0, "/t", 1, lud, ludsz, pp)
        t:check(s3 > s2, "lightuserdata payload 的 props 仍在固定槽 9 生效")
        utils.ud_free(lud)
        utils.ud_free(p2)
        utils.ud_free(p3)
        pp:free()
    end
    do
        local pack, size = mqtt.pack_puback(mqtt.VERSION.V311, 42, 0, nil)
        t:check(pack ~= nil and size > 0, "pack_puback v311")
        t:eq(mqtt.PROT.PUBACK, _ptype(pack, size), "pack_puback type byte")
        utils.ud_free(pack)
        pack, size = mqtt.pack_pubrec(mqtt.VERSION.V311, 42, 0, nil)
        t:eq(mqtt.PROT.PUBREC, _ptype(pack, size), "pack_pubrec type byte")
        utils.ud_free(pack)
        pack, size = mqtt.pack_pubrel(mqtt.VERSION.V311, 42, 0, nil)
        t:eq(mqtt.PROT.PUBREL, _ptype(pack, size), "pack_pubrel type byte")
        utils.ud_free(pack)
        pack, size = mqtt.pack_pubcomp(mqtt.VERSION.V311, 42, 0, nil)
        t:eq(mqtt.PROT.PUBCOMP, _ptype(pack, size), "pack_pubcomp type byte")
        utils.ud_free(pack)
    end

    -- ── pack_subscribe / suback / unsubscribe / unsuback ───────────────
    do
        local topics = mqtt.props()
        topics:subscribe(mqtt.VERSION.V311, "/topic1", 1, 0, 0, 0)
        topics:subscribe(mqtt.VERSION.V311, "/topic2", 2, 0, 0, 0)
        local pack, size = mqtt.pack_subscribe(mqtt.VERSION.V311, 7, topics, nil)
        topics:free()
        t:check(pack ~= nil and size > 0, "pack_subscribe")
        t:eq(mqtt.PROT.SUBSCRIBE, _ptype(pack, size), "pack_subscribe type byte")
        local txt = srey.ud_str(pack, size)
        t:check(txt:find("/topic1", 1, true) ~= nil, "pack_subscribe has /topic1")
        t:check(txt:find("/topic2", 1, true) ~= nil, "pack_subscribe has /topic2")
        utils.ud_free(pack)
    end
    do
        local pack, size = mqtt.pack_suback(mqtt.VERSION.V311, 7, string.char(0, 1), nil)
        t:check(pack ~= nil and size > 0, "pack_suback")
        t:eq(mqtt.PROT.SUBACK, _ptype(pack, size), "pack_suback type byte")
        utils.ud_free(pack)
    end
    do
        local topics = mqtt.props()
        topics:unsubscribe("/topic1")
        local pack, size = mqtt.pack_unsubscribe(mqtt.VERSION.V311, 8, topics, nil)
        topics:free()
        t:check(pack ~= nil and size > 0, "pack_unsubscribe")
        t:eq(mqtt.PROT.UNSUBSCRIBE, _ptype(pack, size), "pack_unsubscribe type byte")
        utils.ud_free(pack)
    end
    do
        local pack, size = mqtt.pack_unsuback(mqtt.VERSION.V50, 8, string.char(0), nil)
        t:check(pack ~= nil and size > 0, "pack_unsuback")
        t:eq(mqtt.PROT.UNSUBACK, _ptype(pack, size), "pack_unsuback type byte")
        utils.ud_free(pack)
    end

    -- ── pack_ping / pong / disconnect / auth ───────────────────────────
    do
        local pack, size = mqtt.pack_ping()
        t:check(pack ~= nil and size == 2, "pack_ping 2 bytes")
        t:eq(mqtt.PROT.PINGREQ, _ptype(pack, size), "pack_ping type byte")
        utils.ud_free(pack)
        pack, size = mqtt.pack_pong()
        t:check(pack ~= nil and size == 2, "pack_pong 2 bytes")
        t:eq(mqtt.PROT.PINGRESP, _ptype(pack, size), "pack_pong type byte")
        utils.ud_free(pack)
        pack, size = mqtt.pack_disconnect(mqtt.VERSION.V311, 0, nil)
        t:check(pack ~= nil and size > 0, "pack_disconnect")
        t:eq(mqtt.PROT.DISCONNECT, _ptype(pack, size), "pack_disconnect type byte")
        utils.ud_free(pack)
        pack, size = mqtt.pack_auth(mqtt.VERSION.V50, 0, nil)
        t:check(pack ~= nil and size > 0, "pack_auth v50")
        t:eq(mqtt.PROT.AUTH, _ptype(pack, size), "pack_auth type byte")
        utils.ud_free(pack)
    end

    -- ── mqtt.reason 转字符串 ───────────────────────────────────────────
    do
        local rs = mqtt.reason(mqtt.PROT.CONNACK, 0)
        t:check(type(rs) == "string" and #rs > 0, "mqtt.reason CONNACK 0")
    end

    -- ── 协议版本的截断回归 ────────────────────────────────────────────
    -- 曾用裸 (mqtt_protversion) 转换：260 让组包侧按 5.0 写属性长度字段，写线时 int8_t
    -- 又截成 0x04，发出去是"协议级别 3.1.1、报文体多一个字节"的畸形 CONNECT
    do
        t:eq(false, pcall(mqttc.pack_connect, 260, 1, 60, "cid"), "pack_connect: version 260 被拒")
        t:eq(false, pcall(mqttc.pack_connect, 3, 1, 60, "cid"), "pack_connect: version 3 被拒")
        -- 挑 pack_disconnect 再验一遍：同一道 _lmqtt_check_version 卡在十个入口上，
        -- 挑一个非 connect 的确认它不是只在 pack_connect 里做了校验
        t:eq(false, pcall(mqttc.pack_disconnect, 0), "pack_disconnect: version 0 被拒")
        t:eq(false, pcall(mqttc.pack_disconnect, 260), "pack_disconnect: version 260 被拒")
        -- 放行的那两条会真的组出包，返回的指针归调用方，丢掉就是泄漏
        local ok, pk = pcall(mqttc.pack_disconnect, 4)
        t:eq(true, ok, "pack_disconnect: MQTT_311 照常接受")
        if ok then utils.ud_free(pk) end
        ok, pk = pcall(mqttc.pack_disconnect, 5)
        t:eq(true, ok, "pack_disconnect: MQTT_50 照常接受")
        if ok then utils.ud_free(pk) end
    end
end)
end)
