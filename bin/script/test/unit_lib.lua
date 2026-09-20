-- lib 层网络封装测试：lib/dns.lua (nslookup) + lib/mqtt.lua (connect)
-- 依赖：DNS 8.8.8.8 可达；test.server_mqtt 已监听 1883——各模块的 startup 由 worker 择时派发、
-- 顺序不定，故开头显式 sleep 等它起来，同 e2e_runner

local srey   = require("lib.srey")
local runner = require("test.runner")
local mqtt   = require("lib.mqtt")
require("lib.dns")

srey.startup(function()
srey.sleep(500)
runner.run(function(t)
    -- ── lib/dns.lua: nslookup (UDP 优先 + TCP 回退) ───────────────────
    do
        -- UDP 路径：默认 nslookup(domain, ipv6=false)，第三个参数 udp=true 优先 UDP
        local ips = nslookup("www.google.com", false, true)
        t:check(ips ~= nil and type(ips) == "table" and #ips > 0,
                "nslookup UDP returns ips")
        if ips and #ips > 0 then
            t:check(type(ips[1]) == "string" and ips[1]:match("^%d+%.%d+%.%d+%.%d+$") ~= nil,
                    "nslookup UDP first ip is ipv4")
        end
    end
    do
        -- TCP 路径：udp=false / 省略走 TCP（默认）
        local ips = nslookup("www.bing.com", false)
        t:check(ips ~= nil and type(ips) == "table" and #ips > 0,
                "nslookup TCP returns ips")
    end
    do
        -- 无效域名应返回 nil（DNS RCODE 非 0）
        local ips = nslookup("nonexist.invalid.tld.srey", false, true)
        t:check(ips == nil or 0 == #ips, "nslookup invalid domain returns nil/empty")
    end

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
