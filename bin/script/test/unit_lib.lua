-- lib 层网络封装测试：lib/mqtt.lua (connect)。lib/dns.lua 的用例在 test.dns_fake（本地假 DNS，不依赖公网）
-- 依赖：test.server_mqtt 已监听 1883——各模块的 startup 由 worker 择时派发、
-- 顺序不定，故开头显式 sleep 等它起来，同 e2e_runner

local srey   = require("lib.srey")
local runner = require("test.runner")
local mqtt   = require("lib.mqtt")

srey.startup(function()
srey.sleep(500)
runner.run(function(t)
    -- ── lib/mqtt.lua: connect (try_connect + wait_connect 同步等待) ───
    do
        -- 连本机 server_mqtt（1883），明文，无握手层应用协议
        local sk = mqtt.connect(mqtt.VERSION.V311, SSL_NAME.NONE, "127.0.0.1", 1883)
        if not sk.valid then
            t:fail("mqtt.connect v3.1.1 to 127.0.0.1:1883")
        else
            t:check(sk.fd > 0, "mqtt.connect v3.1.1 returns a valid sock")
            srey.close(sk)
        end
    end
    do
        local sk = mqtt.connect(mqtt.VERSION.V50, SSL_NAME.NONE, "127.0.0.1", 1883)
        if not sk.valid then
            t:fail("mqtt.connect v5.0 to 127.0.0.1:1883")
        else
            t:check(sk.fd > 0, "mqtt.connect v5.0 returns a valid sock")
            srey.close(sk)
        end
    end
    do
        -- 连接不存在端口失败（127.0.0.1:1 一般不监听）
        local sk = mqtt.connect(mqtt.VERSION.V311, SSL_NAME.NONE, "127.0.0.1", 1)
        t:check(not sk.valid, "mqtt.connect failed port returns invalid sock")
    end
end)
end)
