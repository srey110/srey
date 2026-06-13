-- WebSocket 客户端工具库（wbsk 模块）。
-- 提供：ws:// / wss:// 连接（含 DNS 解析）、帧构造/解包、
-- 分片发送（continua 流）以及 ping/pong/close 控制帧。
-- 依赖：lib.dns（主机名解析）、lib.srey（网络收发）、srey.websock（C 层帧打包）。

require("lib.dns")
local srey     = require("lib.srey")
local websock  = require("srey.websock")
local utils    = require("srey.utils")
local srey_url = require("srey.url")
local PACK_TYPE = PACK_TYPE
local wbsk = {}

-- WebSocket 帧操作码（RFC 6455 §11.8）
---@enum WEBSOCK_PROT
WEBSOCK_PROT = {
    CONTINUA = 0x00,   -- 分片延续帧
    TEXT     = 0x01,   -- 文本帧（UTF-8）
    BINARY   = 0x02,   -- 二进制帧
    CLOSE    = 0x08,   -- 关闭帧
    PING     = 0x09,   -- ping 控制帧
    PONG     = 0x0A    -- pong 控制帧
}

---将操作码转换为可读字符串（调试/日志用）
---@param prot WEBSOCK_PROT
---@return string name "CONTINUA" / "TEXT" / "BINARY" / "CLOSE" / "PING" / "PONG" / "UNKNOWN"
function wbsk.prottostr(prot)
    if WEBSOCK_PROT.CONTINUA == prot then
        return "CONTINUA"
    elseif WEBSOCK_PROT.TEXT == prot  then
        return "TEXT"
    elseif WEBSOCK_PROT.BINARY == prot  then
        return "BINARY"
    elseif WEBSOCK_PROT.CLOSE == prot  then
        return "CLOSE"
    elseif WEBSOCK_PROT.PING == prot  then
        return "PING"
    elseif WEBSOCK_PROT.PONG == prot  then
        return "PONG"
    end
    return "UNKNOWN"
end

---解包 WebSocket 帧
---@type fun(pack:lightuserdata):{ fin:integer, prot:integer, secprot:integer?, secpack:lightuserdata?, data:lightuserdata?, size:integer }
wbsk.unpack = websock.unpack

---解析 ws:// 或 wss:// URL，建立 WebSocket 连接并完成握手
---@param ws string WebSocket URL，如 "ws://host:port/path"
---@param sslname SSL_NAME wss 时必须为有效 SSL 上下文名；ws 时传 SSL_NAME.NONE
---@param secprot string? 子协议名（Sec-WebSocket-Protocol）
---@param netev NET_EV? 事件订阅掩码
---@return integer fd socket fd；任一步失败返回 INVALID_SOCK
---@return integer? skid 连接 skid；仅在 fd 有效时返回
function wbsk.connect(ws, sslname, secprot, netev)
    local url = srey_url.parse(ws)
    if not url then
        return INVALID_SOCK
    end
    if ("ws" ~= url.scheme and "wss" ~= url.scheme) or not url.host then
        return INVALID_SOCK
    end
    if "wss" == url.scheme and SSL_NAME.NONE == sslname then
        return INVALID_SOCK
    end
    -- 主机名需要先通过 DNS 解析为 IP
    local ip = url.host
    if "hostname"  == host_type(url.host)  then
        local ips = nslookup(url.host, false)
        if not ips or 0 == #ips then
            return INVALID_SOCK
        end
        ip = ips[1]
    end
    -- 端口：ws 默认 80，wss 默认 443
    local port = url.port
    if not port then
        if SSL_NAME.NONE == sslname then
            port = 80
        else
            port = 443
        end
    end
    -- 构造 HTTP Upgrade 握手包；signkey 用于 C 层验证服务端 Sec-WebSocket-Accept
    local hspack, size, signkey = websock.pack_handshake(url.host, secprot)
    local fd, skid = srey.connect(PACK_TYPE.WEBSOCK, sslname, ip, port, netev, signkey)
    if INVALID_SOCK == fd then
        utils.ud_free(hspack)   -- TCP 连接失败，释放 C 层分配的握手包内存
        return INVALID_SOCK
    end
    if not srey.send(fd, skid, hspack, size, 0) then
        srey.close(fd, skid)
        return INVALID_SOCK
    end
    -- 等待服务端 101 Switching Protocols（C 层完成验证后触发 HANDSHAKED 消息）
    local ok, data, dlens = srey.wait_handshaked(fd, skid)
    if not ok then
        return INVALID_SOCK
    end
    -- 校验服务端协商的子协议是否与请求一致
    if secprot and #secprot > 0 then
        local got = data and srey.ud_str(data, dlens) or ""
        if got ~= secprot then
            srey.close(fd, skid)
            return INVALID_SOCK
        end
    end
    return fd, skid
end

-- ── 控制帧构造 ────────────────────────────────────────────────────────────

---构造 ping 控制帧（client=1 加掩码）
---@type fun(client:integer):lightuserdata, integer
wbsk.ping = websock.pack_ping

---构造 pong 控制帧（client=1 加掩码）
---@type fun(client:integer):lightuserdata, integer
wbsk.pong = websock.pack_pong

---构造 close 控制帧（触发对端关闭握手）
---@type fun(client:integer):lightuserdata, integer
wbsk.close = websock.pack_close

-- ── 数据帧构造 ────────────────────────────────────────────────────────────

---构造文本帧；fin=1 完整消息，fin=0 分片首帧
---@type fun(client:integer, fin:integer, data:string|lightuserdata, size:integer?):lightuserdata, integer
wbsk.text_fin = websock.pack_text

---构造完整（单帧）文本消息（fin=1）
---@param client integer 1=客户端，0=服务端
---@param data string|lightuserdata 载荷
---@param size integer? data 为 lightuserdata 时必填
---@return lightuserdata frame 数据指针
---@return integer fsize 数据长度
function wbsk.text(client, data, size)
    return wbsk.text_fin(client, 1, data, size)
end

---构造二进制帧（带 fin 标志）
---@type fun(client:integer, fin:integer, data:string|lightuserdata, size:integer?):lightuserdata, integer
wbsk.binary_fin = websock.pack_binary

---构造完整（单帧）二进制消息（fin=1）
---@param client integer 1=客户端，0=服务端
---@param data string|lightuserdata 载荷
---@param size integer? data 为 lightuserdata 时必填
---@return lightuserdata frame 数据指针
---@return integer fsize 数据长度
function wbsk.binary(client, data, size)
    return wbsk.binary_fin(client, 1, data, size)
end

---构造延续帧（CONTINUATION）；fin=1 终止帧可 data=nil
---@type fun(client:integer, fin:integer, data:string|lightuserdata|nil, size:integer?):lightuserdata, integer
wbsk.continua = websock.pack_continua

-- ── 流式分片发送 ──────────────────────────────────────────────────────────

---内部流式发送：将 func(...) 产生的数据按 WebSocket 分片协议逐帧发送；
---发送 fin=1 空 continuation 帧标记消息结束
local function _send_end_frame(fd, skid, client)
    local data, size = wbsk.continua(client, 1, nil, 0)
    return srey.send(fd, skid, data, size, 0)
end

---首帧 text_fin/binary_fin(fin=0)，中间/最后帧用 continua；func 返回 nil 时发 fin=1 终止帧
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param prot WEBSOCK_PROT.TEXT | WEBSOCK_PROT.BINARY
---@param client integer 1=客户端，0=服务端
---@param func fun(...):(string|lightuserdata|nil, integer?) 取数据回调；返回 nil 终止
---@param ... any 传给 func 的额外参数
local function _continua(fd, skid, prot, client, func, ...)
    local data, size = func(...)
    if not data then
        -- 首次返 nil = 空消息,不发任何帧;调用方应自行避免无效调用
        return false
    end
    if WEBSOCK_PROT.TEXT == prot then
        data, size = wbsk.text_fin(client, 0, data, size)
    elseif WEBSOCK_PROT.BINARY == prot then
        data, size = wbsk.binary_fin(client, 0, data, size)
    else
        return false
    end
    if not srey.send(fd, skid, data, size, 0) then
        return false   -- 首帧失败 socket 已坏，不发终止帧
    end
    while true do
        data, size = func(...)
        if data then
            data, size = wbsk.continua(client, 0, data, size)
            if not srey.send(fd, skid, data, size, 0) then
                -- 中间帧失败仍尝试发终止帧让 server 退出 continuation 累积状态
                _send_end_frame(fd, skid, client)
                return false
            end
        else
            -- func 返回 nil：发送 fin=1 的空延续帧标记消息结束
            return _send_end_frame(fd, skid, client)
        end
    end
end

---以 TEXT 分片模式流式发送
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param client integer 1=客户端，0=服务端
---@param func fun(...):(string|lightuserdata|nil, integer?) 取数据回调；返回 nil 终止
---@param ... any 传给 func 的额外参数
---@return boolean ok 是否成功（包括所有帧和终止帧的发送）
function wbsk.text_continua(fd, skid, client, func, ...)
    return _continua(fd, skid, WEBSOCK_PROT.TEXT, client, func, ...)
end

---以 BINARY 分片模式流式发送
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param client integer 1=客户端，0=服务端
---@param func fun(...):(string|lightuserdata|nil, integer?) 取数据回调；返回 nil 终止
---@param ... any 传给 func 的额外参数
---@return boolean ok 是否成功（包括所有帧和终止帧的发送）
function wbsk.binary_continua(fd, skid, client, func, ...)
    return _continua(fd, skid, WEBSOCK_PROT.BINARY, client, func, ...)
end

return wbsk
