-- HTTP 客户端/服务端工具库。
-- 封装 C 层 srey.http 的解包接口，并在其基础上提供：
--   • GET / POST 同步请求（含 chunked 流式响应回调）
--   • HTTP 响应构造
-- 依赖：lib.srey（网络收发）、srey.http（C 层解包）、cjson（JSON 编码）

local srey = require("lib.srey")
local srey_http = require("srey.http")
local json = require("cjson")
local table = table
local string = string
local HTTP_VERSION = "1.1"   -- 固定使用 HTTP/1.1
-- 头名合法性与头部块上限都取自 C，不在这里重抄一份：is_token 是 utils.h 的 RFC 7230 tchar
-- 判定（router.c 组头名用的同一个，非字符串一律 false），max_headlens 是 http.h 的 MAX_HEADLENS。
-- 重抄的话改 C 那边同步不过来，只表现为头静默消失、或整条响应被对端拒收，没人查得到。
-- 业务传来的 key/val 直接拼进报文，含 CR/LF 即可把一条报文劈成两条（响应拆分 / 请求走私），
-- 校验不过按 router.c 的做法整条丢弃 + 告警，不 abort
local is_token = srey_http.is_token
local MAX_HEADLENS = srey_http.max_headlens
-- 组包时反复用到的定长片段。Lua 不对 #"字面量" 做常量折叠，写在循环里就是每头一次取长
local SEP_LEN = #": \r\n"-- 头名与值之间的 ": " 加行尾 CRLF
-- 尾部必写的 "Content-Length: <十进制>" 加两个 CRLF；20 是十进制最长位数
local CL_RESERVE = #"Content-Length: " + 20 + #"\r\n\r\n"
local JSON_CT_LEN = #"Content-Type: application/json\r\n"-- table 分支自带的那行
local http = {}

-- ── 响应解包 ──────────────────────────────────────────────────────────────

---返回状态行 / 请求行三元组：响应为 {version, code, message}，请求为 {method, uri, version}
---@type fun(pack:lightuserdata):string[]|nil
http.status = srey_http.status

---返回报文的分块传输状态：0=非分块；1=首包；2+ 分块中间/结束块
---@type fun(pack:lightuserdata):integer
http.chunked = srey_http.chunked

---按 key 查找单个 HTTP 头部字段值（大小写不敏感）；不存在或分块中间包返回 nil
---@type fun(pack:lightuserdata, key:string):string|nil
http.head = srey_http.head

---返回所有 HTTP 头部字段；分块中间包返回 nil
---@type fun(pack:lightuserdata):table<string,string>|nil
http.heads = srey_http.heads

---返回报文体数据指针和字节数
---@type fun(pack:lightuserdata):lightuserdata|nil, integer
http.data = srey_http.data

---以 Lua 字符串形式返回报文体内容；空时返回 nil
---@type fun(pack:lightuserdata):string|nil
http.datastr = srey_http.datastr

---@class HttpPack
---@field status  string[]?               状态行/请求行三元组（同 http.status 返回值）
---@field chunked integer                 0=非分块；1=首包；2+=中间/结束块
---@field heads   table<string,string>?   响应头 key→value 表；分块中间包为 nil
---@field data    string?                 报文体内容；空时为 nil
---@field cksize  integer?                chunked 模式下累计接收字节数；非 chunked 时不存在

---将整个 HTTP 包解包为 Lua 表
---@param pack lightuserdata http_pack_ctx 指针
---@return HttpPack tb 解包结果
function http.unpack(pack)
    local tb = {}
    tb.status  = srey_http.status(pack)
    tb.chunked = srey_http.chunked(pack)
    tb.heads   = srey_http.heads(pack)
    tb.data    = srey_http.datastr(pack)
    return tb
end
---将数字状态码转换为对应文本描述（如 200 → "OK"）
---@type fun(code:integer):string
http.code_status = srey_http.code_status

-- ── 内部发送/接收 ─────────────────────────────────────────────────────────

---内部发送函数：rsp=true 单向发送（服务端响应），rsp=false 同步发送并接收回包；
---chunked 响应时循环读取分片并通过 ckfunc(fin,data,size) 回调，直到 fin=true
---@param rsp boolean 是否为响应（单向）
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param msg string[] 待拼接的消息片段数组
---@param ckfunc fun(fin:boolean, data:lightuserdata|nil, size:integer)? 分片回调
---@return HttpPack|nil pack 解包后的响应表；失败返回 nil
local function _http_send(rsp, fd, skid, msg, ckfunc)
    local smsg = table.concat(msg)
    if rsp then
        srey.send(fd, skid, smsg, #smsg, 1)
        return
    end
    local pack, _ = srey.syn_send(fd, skid, smsg, #smsg, 1)
    if not pack then
        return
    end
    pack = http.unpack(pack)
    if 1 == pack.chunked then
        pack.cksize = 0
        local ok, data, hdata, hsize, fin
        local chunks
        if not ckfunc then
            chunks = {}
        end
        while true do
            ok, fin, data, _ = srey.syn_slice(fd, skid)
            if not ok then
                return nil
            end
            hdata, hsize = http.data(data)
            if hsize and hsize > 0 then
                pack.cksize = pack.cksize + hsize
                if ckfunc then
                    ckfunc(fin, hdata, hsize)
                else
                    chunks[#chunks + 1] = srey.ud_str(hdata, hsize)
                end
            elseif ckfunc then
                ckfunc(fin, nil, 0)
            end
            if fin then
                break
            end
        end
        if chunks and #chunks > 0 then
            pack.data = table.concat(chunks)
        end
    end
    return pack
end

-- 本函数按 info 类型自己生成的头，调用方不能再传一份：两条 Content-Length（或 TE 叠 CL）
-- 会被 srey 自己的解析器判为请求走私、整包丢弃并断连。命中即整条丢弃 + 告警，
-- 与 C 侧 _router_send_core 同一处置，两个 HTTP 面对同一攻击面给出同一结论。
-- Content-Type 只在 table 分支自动生成，其余分支得靠调用方传，故按 msgtype 分别判
local function _is_auto_head(lk, msgtype, rsp)
    if "content-type" == lk then
        -- 只有 table 分支自己写 Content-Type；其余分支要靠调用方传，不能拦
        return "table" == msgtype
    end
    if "content-length" ~= lk and "transfer-encoding" ~= lk then
        return false
    end
    -- 三个 body 分支都会自己写帧长头，调用方再传就是两条 Content-Length 或 TE 叠 CL。
    -- 无 body 的响应也要拦：要么下面补 Content-Length: 0 会撞车，要么是 1xx/204/304
    -- （nocl），按 RFC 7230 §3.3.2 本就禁止携带。
    -- 唯独"请求 + 无 body"这一种本函数什么都不写，调用方那条是这条请求仅有的帧长头，放行 ——
    -- 一刀切会让 http.post(..., {["Content-Length"]="0"}) 发出既无 CL 也无 TE 的报文
    return "string" == msgtype or "table" == msgtype or "function" == msgtype or rsp
end
---构造并发送 HTTP 消息的核心函数。info 支持 string（带 Content-Length）、
---table（自动 JSON 编码）、function（chunked 流式分块发送，返回 nil 或空串终止流）三种类型
---@param rsp boolean 是否为响应（true=单向，false=同步请求）
---@param nocl boolean 该消息禁止携带 Content-Length（1xx/204/304 响应）；http.response 已在
---       入口把这三类的 info 清掉，故本函数只需在无 body 分支跳过 Content-Length: 0
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param status string 请求行或状态行（已含 \r\n）
---@param headers table<string,any>? 附加头部 key→value 表；本函数按 info 类型自动生成的头
---       （Content-Length、Transfer-Encoding，以及 info 为 table 时的 Content-Type）
---       不得传入，传了整条丢弃并告警——留着会造出两条 Content-Length 或 TE 叠 CL，
---       严格实现与 smuggling 防御代理会拒收。info 非 table 时 Content-Type 正是经此传入
---@param ckfunc fun(fin:boolean, data:lightuserdata|nil, size:integer)? chunked 接收回调
---@param info string|table|fun(...):string?|nil 报文体；string 直接发送，table 自动 JSON 编码，function 流式分块（返回 nil 或空串终止流）
---       function 形态下**不得在回调内挂起**（不要 syn_send / sleep / 等任何消息）：chunked 各块之间
---       让出控制权，别的协程往同一 fd 上发的数据就插进本次报文体中间，对端解析必错。
---       这里没有连接级锁可加——只拿到 fd/skid，不像 pgsql copy_in 那样手里有 ctx 的 serial
---@param ... any 传给 info 函数的额外参数
---@return HttpPack|nil pack 解包后的响应表；rsp=true 或失败时返回 nil
local function _http_msg(rsp, nocl, fd, skid, status, headers, ckfunc, info, ...)
    local msg = {}
    table.insert(msg, status)
    local msgtype = type(info)
    if nil ~= headers then
        -- MAX_HEADLENS 管的是整个头部块，逐条判不够：三条各 2000 字节的头单看都合法，
        -- 拼起来 6000 字节，对端照样整包解析失败。故累计已写入的字节数判定，
        -- 并给后面必写的 "Content-Length: <十进制>" 加两个 CRLF 留出余量；20 是十进制最长位数
        -- （chunked 分支写的 Transfer-Encoding 行比这条短，同一份余量已覆盖）。
        -- 只卡响应侧，与 C 侧一致：那边响应走 _router_send_core 的累计上限，请求走
        -- http_pack_head 完全不限长。请求是发给第三方服务端的（nginx/apache 收 8~16KB），
        -- 拿 srey 自己解析器的 4KB 去卡，只会把 Cookie 之类平白丢掉、而调用方从返回值看不出来；
        -- 对端收不收得下由对端定，超限的后果是拿不到响应，比静默少一条头更容易发现
        local used = 0
        if rsp then
            used = #status + CL_RESERVE
            if "table" == msgtype then
                used = used + JSON_CT_LEN
            end
        end
        for key, val in pairs(headers) do
            local sval = tostring(val)
            if not is_token(key) then
                WARN("http header key is not a valid token, dropped.")
            elseif _is_auto_head(string.lower(key), msgtype, rsp) then
                WARN("http header %s is generated by this function, dropped.", key)
            elseif rsp and used + #key + #sval + SEP_LEN > MAX_HEADLENS then
                WARN("http header %s would push head block past MAX_HEADLENS, dropped.", key)
            elseif string.find(sval, "[\0\r\n]") then
                WARN("http header %s value contains NUL or CRLF, dropped.", key)
            else
                used = used + #key + #sval + SEP_LEN
                local n = #msg
                msg[n + 1] = key
                msg[n + 2] = ": "
                msg[n + 3] = sval
                msg[n + 4] = "\r\n"
            end
        end
    end
    if "string" == msgtype then
        table.insert(msg, string.format("Content-Length: %d\r\n\r\n", #info))
        table.insert(msg, info)
        return _http_send(rsp, fd, skid, msg, ckfunc)
    elseif "table" == msgtype then
        local jmsg = json.encode(info)
        table.insert(msg, string.format("Content-Type: application/json\r\nContent-Length: %d\r\n\r\n", #jmsg))
        table.insert(msg, jmsg)
        return _http_send(rsp, fd, skid, msg, ckfunc)
    elseif "function" == msgtype then
        -- 流式分块发送：每次调用 info(...) 取一块数据，拼成 chunked 格式后发送，
        -- info 返回 nil 或空串时结束流并补发终止块。
        table.insert(msg, "Transfer-Encoding: chunked\r\n\r\n")
        local smsg, rtn
        while true do
            rtn = info(...)
            if nil == rtn then
                break
            end
            if "string" ~= type(rtn) then
                ERROR("chunked function must return string, got %s.", type(rtn))
                -- 生产者违约,body 已截断:补终止块让对端退出 chunked 累积状态,但按失败返回,
                -- 不把截断的 body 当成一次完整请求交回调用方。
                -- msg 未发送时(首次迭代)一并带上头部,避免对端收到孤立的终止块
                table.insert(msg, "0\r\n\r\n")
                -- 补了终止块后这仍是一条语法完整的 chunked 请求,对端必回响应,故走正常发送路径把它收完再丢弃:
                -- 响应自身也可能是 chunked(只 syn_send 收不干净),而 _wait_net_recv 按 skid 匹配、不区分请求,
                -- 漏收就会落到下一次请求注册的等待者上,连接从此错位一格。ckfunc 传 nil:不拿要丢弃的数据回调业务
                _http_send(rsp, fd, skid, msg, nil)
                return
            end
            -- 空串按结束处理：既是 chunked 终止块的语义，也避免此处零状态推进死循环
            if 0 == #rtn then
                break
            end
            table.insert(msg, string.format("%x\r\n", #rtn))
            table.insert(msg, rtn)
            table.insert(msg, "\r\n")
            smsg = table.concat(msg)
            if not srey.send(fd, skid, smsg, #smsg, 1) then
                -- 中间帧失败仍尝试补发终止块,让对端退出 chunked 累积状态
                srey.send(fd, skid, "0\r\n\r\n", 5, 1)
                return
            end
            for i = #msg, 1, -1 do
                msg[i] = nil
            end
        end
        table.insert(msg, "0\r\n\r\n")
        return _http_send(rsp, fd, skid, msg, ckfunc)
    elseif rsp and not nocl then
        -- 响应无 body 必须显式 Content-Length: 0（RFC 7230 §3.3.3 规则 7：响应缺 CL/TE 时
        -- body 由连接关闭界定，keep-alive 下合规客户端会一直读到关闭才认为响应结束）。
        -- C 侧 http_pack_content 对空 body 也是统一写 Content-Length: 0
        table.insert(msg, "Content-Length: 0\r\n\r\n")
        return _http_send(rsp, fd, skid, msg, ckfunc)
    else
        -- 请求无 body 不带 CL/TE 即可（同规则 6）；1xx/204/304 响应则是禁止带（见 http.response）
        table.insert(msg, "\r\n")
        return _http_send(rsp, fd, skid, msg, ckfunc)
    end
end

-- 拼请求行前校验 url。不校验就是 HTTP 请求拆分：url 里塞一段 CRLF 能在同一条连接上
-- 再拼出一个完整请求，多出来的那条响应会被下一次请求的等待者取走，此后整条连接错开一位。
-- 头名头值那边是丢弃单条继续发，请求目标坏了整条请求都不能发，故返回 nil 让调用方失败返回。
-- C 侧 http_pack_req 把这视为硬契约违约直接 ASSERTAB，Lua 这条路径此前是静默放行
local function _req_status(method, url)
    url = url or "/"
    if string.find(url, "[\0\r\n]") then
        WARN("http url contains NUL or CRLF, request dropped.")
        return nil
    end
    return string.format("%s %s HTTP/%s\r\n", method, url, HTTP_VERSION)
end

-- ── 公共 API ──────────────────────────────────────────────────────────────

---同步 GET 请求
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param url string? URL 路径，默认 "/"；含 NUL/CRLF 时整条请求被拒（HTTP 请求拆分）
---@param headers table<string,any>? 附加头部
---@param ckfunc fun(fin:boolean, data:lightuserdata|nil, size:integer)? chunked 接收回调
---@return HttpPack|nil pack 解包后的响应表；失败返回 nil（超时/分片中断时响应可能未收完，
---此连接不应继续复用，应关闭——等待按 skid 匹配，残留响应会被下一次请求错认）。
---url 非法时同样返回 nil，但一个字节都没发出，连接可继续复用
function http.get(fd, skid, url, headers, ckfunc)
    local status = _req_status("GET", url)
    if not status then
        return nil
    end
    return _http_msg(false, false, fd, skid, status, headers, ckfunc)
end

---同步 POST 请求；info 为报文体（string/table/function），用法同 _http_msg
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param url string? URL 路径，默认 "/"；含 NUL/CRLF 时整条请求被拒（HTTP 请求拆分）
---@param headers table<string,any>? 附加头部
---@param ckfunc fun(fin:boolean, data:lightuserdata|nil, size:integer)? chunked 接收回调
---@param info string|table|fun(...):string?|nil 报文体；string 直接发送，table 自动 JSON 编码，function 流式分块（返回 nil 或空串终止流）
---       function 形态下**不得在回调内挂起**（不要 syn_send / sleep / 等任何消息）：chunked 各块之间
---       让出控制权，别的协程往同一 fd 上发的数据就插进本次报文体中间，对端解析必错。
---       这里没有连接级锁可加——只拿到 fd/skid，不像 pgsql copy_in 那样手里有 ctx 的 serial
---@param ... any 传给 info 函数的额外参数
---@return HttpPack|nil pack 解包后的响应表；失败返回 nil（超时/分片中断时响应可能未收完，
---此连接不应继续复用，应关闭——等待按 skid 匹配，残留响应会被下一次请求错认）。
---url 非法时同样返回 nil，但一个字节都没发出，连接可继续复用
function http.post(fd, skid, url, headers, ckfunc, info, ...)
    local status = _req_status("POST", url)
    if not status then
        return nil
    end
    return _http_msg(false, false, fd, skid, status, headers, ckfunc, info, ...)
end

---向客户端发送 HTTP 响应（单向，不等待回包）
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param code integer 状态码（如 200、404）
---@param headers table<string,any>? 附加头部
---@param info string|table|fun(...):string?|nil 报文体；string 直接发送，table 自动 JSON 编码，function 流式分块（返回 nil 或空串终止流）
---       function 形态下**不得在回调内挂起**（不要 syn_send / sleep / 等任何消息）：chunked 各块之间
---       让出控制权，别的协程往同一 fd 上发的数据就插进本次报文体中间，对端解析必错。
---       这里没有连接级锁可加——只拿到 fd/skid，不像 pgsql copy_in 那样手里有 ctx 的 serial
---@param ... any 传给 info 函数的额外参数
function http.response(fd, skid, code, headers, info, ...)
    local status = string.format("HTTP/%s %03d %s\r\n", HTTP_VERSION, code, http.code_status(code))
    -- RFC 7230 §3.3.2：1xx 与 204 一律禁止带 Content-Length；304 允许带，但那个值应当反映
    -- 实体的真实长度，这里根本没有实体，补 Content-Length: 0 等于谎报资源为空，故一并跳过。
    -- 严格代理会因此丢弃或重置这类响应，所以不能对所有无 body 响应无差别补 CL
    local nocl = code < 200 or 204 == code or 304 == code
    if nocl then
        info = nil-- 这三类响应同样禁带报文体：给了也丢，与 C 侧 _router_send_core 同口径
    end
    _http_msg(true, nocl, fd, skid, status, headers, nil, info, ...)
end

return http
