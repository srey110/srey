-- lib 层网络封装测试：lib/dns.lua (nslookup) + lib/mqtt.lua (connect)
-- 依赖：DNS 8.8.8.8 可达；test.server_mqtt 已监听 1883

local srey   = require("lib.srey")
local runner = require("test.runner")
local mqtt   = require("lib.mqtt")
require("lib.dns")

srey.startup(function()
runner.run("lib", function(t)
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
        local fd, skid = mqtt.connect(mqtt.VERSION.V311, SSL_NAME.NONE, "127.0.0.1", 1883)
        if INVALID_SOCK == fd then
            t:fail("mqtt.connect v3.1.1 to 127.0.0.1:1883")
        else
            t:check(fd > 0 and skid ~= nil, "mqtt.connect v3.1.1 returns fd+skid")
            srey.close(fd, skid)
        end
    end
    do
        local fd, skid = mqtt.connect(mqtt.VERSION.V50, SSL_NAME.NONE, "127.0.0.1", 1883)
        if INVALID_SOCK == fd then
            t:fail("mqtt.connect v5.0 to 127.0.0.1:1883")
        else
            t:check(true, "mqtt.connect v5.0 ok")
            srey.close(fd, skid)
        end
    end
    do
        -- 连接不存在端口失败（127.0.0.1:1 一般不监听）
        local fd = mqtt.connect(mqtt.VERSION.V311, SSL_NAME.NONE, "127.0.0.1", 1)
        t:eq(INVALID_SOCK, fd, "mqtt.connect failed port returns INVALID_SOCK")
    end
end)
end)
