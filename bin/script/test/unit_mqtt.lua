-- mqtt 绑定层单元测试：pack_* 系列 wire format 首字节验证 + props/topics 写入

local srey   = require("lib.srey")
local runner = require("test.runner")
local utils  = require("srey.utils")
local mqtt   = require("lib.mqtt")

-- 取 pack 返回数据的首字节高 4 位（MQTT 控制类型）
local function _ptype(pack, size)
    if not pack or 0 == size then return -1 end
    local b = string.byte(srey.ud_str(pack, 1))
    return (b >> 4) & 0x0F
end

srey.startup(function()
runner.run("mqtt", function(t)
    -- ── props 写入 ─────────────────────────────────────────────────────
    do
        local p = mqtt.props()
        p:fixnum(mqtt.PROP.SESSION_EXPIRY, 120)
        p:fixnum(mqtt.PROP.RECEIVE_MAXIMUM, 15000)
        p:kv(mqtt.PROP.USER_PROPERTY, "key1", "val1")
        p:varnum(mqtt.PROP.SUBSCRIPTION_ID, 99)
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
        local pack, size = mqtt.pack_connack(mqtt.VERSION.V311, 1, 0, nil)
        t:check(pack ~= nil and size > 0, "pack_connack v311")
        t:eq(mqtt.PROT.CONNACK, _ptype(pack, size), "pack_connack type byte")
        utils.ud_free(pack)
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
end)
end)
