-- lib.websock 客户端两处修复:
--   1) scheme 大小写无关(RFC 3986 §3.1,C 侧 coro_utils 用 buf_icompare):"WS://" 必须能连上,
--      且默认端口不能因大小写落错分支;
--   2) text_continua 的生产者返回非 string / userdata 缺 size 是违约,须记 ERROR 并返回 false,
--      不可当成正常流结束——那会把截断的消息以 fin=1 收尾,对端当成一条完整消息。
-- 同一 task 内起 WEBSOCK 监听(C 层自动完成升级握手)再连回本机,server 侧不回任何数据。

local srey   = require("lib.srey")
local runner = require("test.runner")
local wbsk   = require("lib.websock")

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

    srey.unlisten(lid)-- 释放端口给后续测试
end)
end)
