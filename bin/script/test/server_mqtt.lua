-- MQTT 测试 broker（监听 1883），行为对齐 C 层 test/task_mqtt_server.c，
-- 供 bin/py_assist/test_mqtt.py e2e 用例和 lua client (test.unit_lib) 直接连接。
-- 协议：处理 CONNECT/PUBLISH/PUBREL/SUBSCRIBE/UNSUBSCRIBE/PINGREQ/DISCONNECT/AUTH。

local srey = require("lib.srey")
local mqtt = require("lib.mqtt")

local _PORT = 1883

-- QoS1/2 PUBLISH 计数器，循环覆盖三种 PUBACK reason code 分支
local _publish = 0

local function _send(fd, skid, pack, size)
    if pack then srey.send(fd, skid, pack, size, 0) end
end

local function _handle(_, fd, skid, _, _, data, _)
    local prot = mqtt.prot(data)
    local ver = mqtt.pack_version(data)
    if mqtt.PROT.CONNECT == prot then
        local info = mqtt.connect_info(data)
        if info then
            printd("mqtt connect: clientid=%s keepalive=%d", info.clientid or "", info.keepalive or 0)
        end
        local props
        if ver >= mqtt.VERSION.V50 then
            props = mqtt.props()
            props:fixnum(mqtt.PROP.SESSION_EXPIRY, 120)
            props:fixnum(mqtt.PROP.RECEIVE_MAXIMUM, 15000)
            props:fixnum(mqtt.PROP.MAXIMUM_QOS, 1)
            props:kv(mqtt.PROP.USER_PROPERTY, "key1", "val1")
            props:kv(mqtt.PROP.USER_PROPERTY, "key2", "val2")
        end
        local pk, sz = mqtt.pack_connack(ver, 1, 0, props)
        if props then props:free() end
        _send(fd, skid, pk, sz)
    elseif mqtt.PROT.PUBLISH == prot then
        local _, qos, _, packid, _, content = mqtt.publish(data)
        if "bye" == content then
            local pk, sz = mqtt.pack_disconnect(ver, 0, nil)
            _send(fd, skid, pk, sz)
            return
        end
        if 0 == qos then return end
        _publish = _publish + 1
        local pk, sz
        if 1 == qos then
            if 1 == _publish then
                local props = mqtt.props()
                props:kv(mqtt.PROP.USER_PROPERTY, "key1", "val1")
                pk, sz = mqtt.pack_puback(ver, packid, 0, props)
                props:free()
            elseif 2 == _publish then
                pk, sz = mqtt.pack_puback(ver, packid, 0x10, nil)
            else
                pk, sz = mqtt.pack_puback(ver, packid, 0, nil)
            end
        elseif 2 == qos then
            if 1 == _publish then
                local props = mqtt.props()
                props:kv(mqtt.PROP.USER_PROPERTY, "key1", "val1")
                pk, sz = mqtt.pack_pubrec(ver, packid, 0, props)
                props:free()
            else
                pk, sz = mqtt.pack_pubrec(ver, packid, 0, nil)
            end
        end
        _send(fd, skid, pk, sz)
        if 3 == _publish then _publish = 0 end
    elseif mqtt.PROT.PUBREL == prot then
        local packid = mqtt.pubrel(data)
        local pk, sz = mqtt.pack_pubcomp(ver, packid, 0, nil)
        _send(fd, skid, pk, sz)
    elseif mqtt.PROT.SUBSCRIBE == prot then
        local packid, topics = mqtt.subscribe(data)
        printd("mqtt subscribe: %d topic(s), first=%s", #topics, topics[1] and topics[1].topic or "")
        local reasons = string.char(0)
        local props = mqtt.props()
        props:kv(mqtt.PROP.USER_PROPERTY, "key1", "val1")
        local pk, sz = mqtt.pack_suback(ver, packid, reasons, props)
        props:free()
        _send(fd, skid, pk, sz)
        -- 主动推 QoS0 publish，覆盖 server -> client 路径
        pk, sz = mqtt.pack_publish(ver, 0, 0, 0, "/test/topic1", 0, "server push")
        _send(fd, skid, pk, sz)
    elseif mqtt.PROT.UNSUBSCRIBE == prot then
        local packid, topics = mqtt.unsubscribe(data)
        printd("mqtt unsubscribe: %d topic(s), first=%s", #topics, topics[1] or "")
        local props = mqtt.props()
        props:kv(mqtt.PROP.USER_PROPERTY, "key1", "val1")
        local pk, sz = mqtt.pack_unsuback(ver, packid, string.char(0), props)
        props:free()
        _send(fd, skid, pk, sz)
    elseif mqtt.PROT.PINGREQ == prot then
        local pk, sz = mqtt.pack_pong()
        _send(fd, skid, pk, sz)
    -- DISCONNECT / AUTH 不需要回复
    end
end

srey.startup(function()
    srey.on_recved(_handle)
    if ERR_FAILED == srey.listen(PACK_TYPE.MQTT, SSL_NAME.NONE, "0.0.0.0", _PORT) then
        WARN("server_mqtt listen %d error", _PORT)
        return
    end
    printd("server_mqtt listening on %d", _PORT)
end)
