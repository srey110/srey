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

---解析 ws:// / wss:// URL 并校验 scheme 与 SSL 配套
---@param ws string WebSocket URL
---@param sslname SSL_NAME wss 时必须为有效 SSL 上下文名
---@return ParsedURL? url scheme 非 ws/wss、缺 host 或 wss 未配 SSL 时返回 nil
local function _parse_url(ws, sslname)
    local url = srey_url.parse(ws, false)
    if not url then
        return nil
    end
    -- RFC 3986 §3.1:scheme 大小写无关。url_parse 原样切出不归一化,C 侧 coro_utils 是 buf_icompare 比的,
    -- 故此处先转小写——下游 _resolve_addr / _reorg 拿的是同一个表,默认端口那两处比较随之对齐
    url.scheme = url.scheme and url.scheme:lower()
    if ("ws" ~= url.scheme and "wss" ~= url.scheme) or not url.host then
        return nil
    end
    if "wss" == url.scheme and SSL_NAME.NONE == sslname then
        return nil
    end
    return url
end

---host 解析为连接用 IP（主机名走 DNS），端口取 url 显式值或按 scheme 默认（RFC 6455 §3：ws 80 / wss 443）
---@param url ParsedURL
---@return string? ip 连接地址；DNS 解析失败或端口非法返回 nil
---@return integer? port
local function _resolve_addr(url)
    local ip = url.host
    if "[" == ip:sub(1, 1) and "]" == ip:sub(-1) then
        ip = ip:sub(2, -2)-- IPv6 字面量按 RFC 3986 §3.2.2 带方括号,连接地址须剥离(Host 头仍用原始形式)
    elseif "hostname" == host_type(ip) then
        local ips = nslookup(ip, false)
        if not ips or 0 == #ips then
            return nil
        end
        ip = ips[1]
    end
    local port
    if url.port then
        if not url.port:match("^%d+$") then
            return nil
        end
        port = tonumber(url.port)
        if port < 1 or port > 65535 then
            return nil
        end
    else
        port = ("wss" == url.scheme) and 443 or 80
    end
    return ip, port
end

---Host 头补非默认端口，path + query 重组为 HTTP request-target
---@param url ParsedURL
---@param port integer 已规范化的端口（_resolve_addr 输出）
---@return string host_hdr Host 头值
---@return string uri request-target
local function _reorg(url, port)
    -- Host 头须带非默认端口(RFC 6455 §4.1),否则严格服务端 / vhost 路由按纯主机名拒握手。
    -- 按数值而非字符串比较:ws://h:080 的端口语义就是 80,等于默认值不该加后缀(与 C 侧 _ws_reorg 一致)
    local host_hdr = url.host
    if url.port and port ~= (("wss" == url.scheme) and 443 or 80) then
        host_hdr = url.host .. ":" .. port
    end
    local path = url.path or "/"
    local uri = url.query and (path .. "?" .. url.query) or path
    return host_hdr, uri
end

---打 HTTP Upgrade 握手包并连接，发出后等服务端响应；hsctx 交 C 层验证 Sec-WebSocket-Accept 与子协议
---@param sslname SSL_NAME SSL 上下文名
---@param ip string 连接地址
---@param port integer 连接端口
---@param netev NET_EV? 事件订阅掩码
---@param host_hdr string Host 头值
---@param uri string request-target
---@param secprot string? 子协议名，可逗号分隔多个
---@return integer fd socket fd；任一步失败返回 INVALID_SOCK
---@return integer? skid 连接 skid；仅在 fd 有效时返回
---@return lightuserdata? spctx 协商到的子协议(ws_secprots_ctx)
local function _handshake(sslname, ip, port, netev, host_hdr, uri, secprot)
    local hspack, size, hsctx = websock.pack_handshake(host_hdr, uri, secprot)
    if not hspack then
        return INVALID_SOCK
    end
    local fd, skid = srey.connect(PACK_TYPE.WEBSOCK, sslname, ip, port, netev, hsctx)
    if INVALID_SOCK == fd then
        utils.ud_free(hspack)   -- TCP 连接失败，释放 C 层分配的握手包内存
        return INVALID_SOCK
    end
    if not srey.send(fd, skid, hspack, size, 0) then
        srey.close(fd, skid)
        return INVALID_SOCK
    end
    -- 等待服务端 101 Switching Protocols（C 层完成验证后触发 HANDSHAKED 消息）
    local ok, spctx = srey.wait_handshaked(fd, skid)
    if not ok then
        return INVALID_SOCK
    end
    return fd, skid, spctx
end

---解析 ws:// 或 wss:// URL，建立 WebSocket 连接并完成握手
---@param ws string WebSocket URL，如 "ws://host:port/path"
---@param sslname SSL_NAME wss 时必须为有效 SSL 上下文名；ws 时传 SSL_NAME.NONE
---@param secprot string? 子协议名（Sec-WebSocket-Protocol），可逗号分隔多个
---@param netev NET_EV? 事件订阅掩码
---@return integer fd socket fd；任一步失败返回 INVALID_SOCK
---@return integer? skid 连接 skid；仅在 fd 有效时返回
---@return lightuserdata? spctx 协商到的子协议(ws_secprots_ctx)，用 websock.secprots 解析；未协商/降级为 nil；仅本协程下次挂起前有效
function wbsk.connect(ws, sslname, secprot, netev)
    local url = _parse_url(ws, sslname)
    if not url then
        return INVALID_SOCK
    end
    local ip, port = _resolve_addr(url)
    if not ip then
        return INVALID_SOCK
    end
    local host_hdr, uri = _reorg(url, port)
    return _handshake(sslname, ip, port, netev, host_hdr, uri, secprot)
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

---判定 func 取到的数据块：nil / 空 string / size 为 0 是正常流结束；
---非 string 非 userdata、或 userdata 缺 size(长度无从得知)是生产者违约，不可当结束——
---那会把截断的消息以 fin=1 收尾交给对端，当成一条完整消息
---@param data string|lightuserdata|nil
---@param size integer?
---@return "data"|"end"|"bad" state
local function _blk_state(data, size)
    if nil == data then
        return "end"
    end
    if "string" == type(data) then
        return (0 == #data) and "end" or "data"
    end
    if "userdata" ~= type(data) or nil == size then
        return "bad"
    end
    return (0 == size) and "end" or "data"
end

---内部流式发送：将 func(...) 产生的数据按 WebSocket 分片协议逐帧发送；
---发送 fin=1 空 continuation 帧标记消息结束
local function _send_end_frame(fd, skid, client)
    local data, size = wbsk.continua(client, 1, "", 0)
    return srey.send(fd, skid, data, size, 0)
end

---首帧 text_fin/binary_fin(fin=0)，中间/最后帧用 continua；func 返回 nil 或空块时发 fin=1 终止帧
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param prot WEBSOCK_PROT.TEXT | WEBSOCK_PROT.BINARY
---@param client integer 1=客户端，0=服务端
---@param func fun(...):(string|lightuserdata|nil, integer?) 取数据回调；返回 nil 或空块终止
---@param ... any 传给 func 的额外参数
local function _continua(fd, skid, prot, client, func, ...)
    local data, size = func(...)
    local state = _blk_state(data, size)
    if "bad" == state then
        ERROR("websock continua func must return string or (lightuserdata, size), got %s.", type(data))
        return false-- 尚未发出任何帧,对端无 continuation 状态可清
    end
    if "end" == state then
        -- 首次返 nil 或空块 = 空消息,不发任何帧;调用方应自行避免无效调用
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
        state = _blk_state(data, size)
        if "bad" == state then
            -- 违约:消息已发出部分帧,仍补终止帧让对端退出 continuation 累积状态,但按失败返回,
            -- 不能沿用 "end" 分支的成功语义——那等于把截断的消息当完整消息交付
            ERROR("websock continua func must return string or (lightuserdata, size), got %s.", type(data))
            _send_end_frame(fd, skid, client)
            return false
        end
        -- 空块(含 nil)即结束：发 fin=1 空延续帧标记消息结束。
        -- 不可仅跳过空块继续循环——零状态推进会无限发 fin=0 空帧
        if "end" == state then
            return _send_end_frame(fd, skid, client)
        end
        data, size = wbsk.continua(client, 0, data, size)
        if not srey.send(fd, skid, data, size, 0) then
            -- 中间帧失败仍尝试发终止帧让 server 退出 continuation 累积状态
            _send_end_frame(fd, skid, client)
            return false
        end
    end
end

---以 TEXT 分片模式流式发送
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param client integer 1=客户端，0=服务端
---@param func fun(...):(string|lightuserdata|nil, integer?) 取数据回调；返回 nil 或空块终止；
---lightuserdata 必须同时给出 size，返回其他类型或缺 size 记 ERROR 并按失败返回（消息已截断）
---@param ... any 传给 func 的额外参数
---@return boolean ok 是否成功（包括所有帧和终止帧的发送）；func 违约时为 false
function wbsk.text_continua(fd, skid, client, func, ...)
    return _continua(fd, skid, WEBSOCK_PROT.TEXT, client, func, ...)
end

---以 BINARY 分片模式流式发送
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param client integer 1=客户端，0=服务端
---@param func fun(...):(string|lightuserdata|nil, integer?) 取数据回调；返回 nil 或空块终止；
---lightuserdata 必须同时给出 size，返回其他类型或缺 size 记 ERROR 并按失败返回（消息已截断）
---@param ... any 传给 func 的额外参数
---@return boolean ok 是否成功（包括所有帧和终止帧的发送）；func 违约时为 false
function wbsk.binary_continua(fd, skid, client, func, ...)
    return _continua(fd, skid, WEBSOCK_PROT.BINARY, client, func, ...)
end

return wbsk
