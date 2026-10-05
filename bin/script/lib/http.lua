-- HTTP 客户端/服务端工具库。
-- 封装 C 层 srey.http 的解包接口，并在其基础上提供：
--   • GET / POST 同步请求（含 chunked 流式响应回调）
--   • HTTP 响应构造
-- 依赖：lib.srey（网络收发）、srey.http（C 层解包）、yyjson（JSON 编码）

local srey = require("lib.srey")
local core = require("srey.core")
local srey_http = require("srey.http")
local json = require("yyjson")
local SLICE_TYPE = SLICE_TYPE
local table = table
local string = string
local type = type
local pairs = pairs
local HTTP_VERSION = "1.1" -- 固定使用 HTTP/1.1
local REQ_TAIL = " HTTP/" .. HTTP_VERSION .. "\r\n" -- 请求行 url 之后的固定部分
-- 头名 / 头值校验在 C 里一次判完（head_check，判据同 router.c 组头那份）。校验不过按 router.c 的
-- 做法整条丢弃 + 告警，不 abort。头部块总长不卡：HTTP_MAX_HEADLENS 只作用于接收侧
local head_check = srey_http.head_check
local AUTO_CT = 0x01 -- head_check 的 autoflags：Content-Type 由本模块自己写
local AUTO_FRAME = 0x02 -- 同上：Content-Length / Transfer-Encoding 由本模块自己写
local http_respond = srey_http.respond
-- 状态行按状态码缓存。只缓存 [100, 999] 的整数码：业务传的怪码不进表，表就撑不大
local _status_lines = {}
local http = {}

-- ── 响应解包 ──────────────────────────────────────────────────────────────

---返回状态行 / 请求行三元组：响应为 {version, code, message}，请求为 {method, uri, version}
---@type fun(pack:lightuserdata):string[]|nil
http.status = srey_http.status

---返回报文的 Transfer-Encoding: chunked 状态：0=非分块；1=首包；2+ 分块中间/结束块。
---只认 Transfer-Encoding：响应既无 Content-Length 又无 TE 时（RFC 7230 §3.3.3 规则 7，
---body 由连接关闭界定）本值仍是 0，而 body 确实是按分片投的。判"还有没有后续"要看
---on_recved 的 slice 形参或 syn_send / syn_recv 的第三返回值，别用本值
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
---@field chunked integer                 Transfer-Encoding: chunked 状态，取值与判定注意事项同 http.chunked
---@field heads   table<string,string>?   响应头 key→value 表；分块中间包为 nil
---@field data    string?                 报文体内容；空时为 nil
---@field cksize  integer?                chunked 模式下累计接收字节数；非 chunked 时不存在

-- http.unpack 的本体：状态行由调用方给（客户端判 1xx 时已取过一次），构造器一次建好四个字段
local function _unpack(pack, st)
    return {
        status  = st,
        chunked = srey_http.chunked(pack),
        heads   = srey_http.heads(pack),
        data    = srey_http.datastr(pack),
    }
end
---将整个 HTTP 包解包为 Lua 表
---@param pack lightuserdata http_pack_ctx 指针
---@return HttpPack tb 解包结果
function http.unpack(pack)
    return _unpack(pack, srey_http.status(pack))
end
---将数字状态码转换为对应文本描述（如 200 → "OK"）
---@type fun(code:integer):string
http.code_status = srey_http.code_status

-- ── 内部发送/接收 ─────────────────────────────────────────────────────────

---取状态行，并判它是不是 1xx 中间响应。RFC 7231 §6.2：最终响应还在后面，客户端必须容忍任意条
---@param pack lightuserdata http_pack_ctx 指针
---@return string[]|nil st 状态行三元组，同 http.status
---@return boolean interim
local function _http_interim(pack)
    local st = srey_http.status(pack)
    local code = st and tonumber(st[2])
    return st, nil ~= code and code >= 100 and code < 200
end

---内部发送函数：rsp=true 单向发送（服务端响应），rsp=false 同步发送并接收回包；
---chunked 响应时循环读取分片并通过 ckfunc(fin,data,size) 回调，直到 fin=true
---@param rsp boolean 是否为响应（单向）
---@param sk userdata 连接标识
---@param smsg string 整条待发报文
---@param ckfunc fun(fin:boolean, data:lightuserdata|nil, size:integer)? 分片回调
---@return HttpPack|nil pack 解包后的响应表；失败返回 nil
local function _http_send(rsp, sk, smsg, ckfunc)
    if rsp then
        srey.send(sk, smsg, #smsg, 1)
        return
    end
    local pack, _, slice = srey.syn_send(sk, smsg, #smsg, 1)
    -- 1xx 是中间响应，C 侧当独立完整消息投出(slice=0)：不跳过就会把它当结果返回，
    -- 真正的响应留在连接上被下一次请求取走。HEAD 的 INIT_NOBODY 登记 C 侧跨 1xx 保留
    local st, interim
    while pack do
        st, interim = _http_interim(pack)
        if not interim then
            break
        end
        pack, _, slice = srey.syn_recv(sk)
    end
    if not pack then
        return
    end
    pack = _unpack(pack, st)
    -- 按协议层给的分片标记决定要不要接着收，不只认 chunked：响应既无 Content-Length 又无
    -- Transfer-Encoding 时 body 由连接关闭界定(RFC 7230 §3.3.3 规则 7)，C 侧同样按分片投、
    -- 末片由关闭事件补。判定留在 C 一处，这里重抄一遍必然分叉(1xx/204/304 也没有 CL/TE)
    if SLICE_TYPE.START == slice then
        local ok, data, hdata, hsize, hstr, fin
        local cksize, nchunk = 0, 0
        local chunks
        if not ckfunc then
            chunks = {}
        end
        while true do
            ok, fin, data, _ = srey.syn_slice(sk)
            if not ok then
                return nil
            end
            if ckfunc then
                hdata, hsize = http.data(data)
                if hsize and hsize > 0 then
                    cksize = cksize + hsize
                    ckfunc(fin, hdata, hsize)
                else
                    ckfunc(fin, nil, 0)
                end
            else
                -- 不回调就直接取成串收着，省一次 C 调用；空片 datastr 返回 nil
                hstr = srey_http.datastr(data)
                if nil ~= hstr then
                    cksize = cksize + #hstr
                    nchunk = nchunk + 1
                    chunks[nchunk] = hstr
                end
            end
            if fin then
                break
            end
        end
        pack.cksize = cksize
        if nchunk > 0 then
            pack.data = table.concat(chunks)
        end
    end
    return pack
end

-- 本函数按 info 类型自己生成的头，调用方不能再传一份：两条 Content-Length（或 TE 叠 CL）
-- 会被 srey 自己的解析器判为请求走私、整包丢弃并断连。命中即整条丢弃 + 告警，
-- 与 C 侧 _router_send_core 同一处置。Content-Type 只有 table 分支自己写；帧长头三个 body
-- 分支都自己写，无 body 的响应也补 Content-Length: 0，唯独"请求 + 无 body"什么都不写、放行
local function _auto_flags(msgtype, rsp)
    if "table" == msgtype then
        return AUTO_CT | AUTO_FRAME
    end
    if "string" == msgtype or "function" == msgtype or rsp then
        return AUTO_FRAME
    end
    return 0
end
-- head_check 没通过时按结果码告警。头值只收 string 与 number：别的类型要过 __tostring,
-- 抛出来就越过了整条校验链，而这一族的契约是丢头加告警、从不抛
local function _head_warn(rc, key)
    if 1 == rc then
        WARN("http header key is not a valid token, dropped.")
    elseif 2 == rc then
        WARN("http header %s value must be a string or number, dropped.", key)
    elseif 3 == rc then
        WARN("http header %s is generated by this function, dropped.", key)
    else
        WARN("http header %s value contains NUL or CRLF, dropped.", key)
    end
end
---构造并发送 HTTP 消息的核心函数。info 支持 string（带 Content-Length）、
---table（自动 JSON 编码）、function（chunked 流式分块发送，返回 nil 或空串终止流）三种类型
---@param rsp boolean 是否为响应（true=单向，false=同步请求）
---@param nocl boolean 该消息禁止携带 Content-Length（1xx/204/304 响应）；http.response 已在
---       入口把这三类的 info 清掉，故本函数只需在无 body 分支跳过 Content-Length: 0
---@param headonly boolean 回 HEAD 请求：头与同一资源的 GET 逐字节一致（RFC 7231 §4.3.2），
---       只是不写报文体。故只在各 body 分支跳过写 body 那一步，帧长头照原样算——Content-Length
---       写真实长度（写 0 等于谎报资源为空），多发的字节则会被对端当成下一条响应的开头。
---       body 是生产者函数时长度未知，省掉 Content-Length 但照发 Transfer-Encoding，
---       且不去跑那个生产者（HEAD 响应到空行即终止，带着 chunked 也不会让对端接着等块）
---@param sk userdata 连接标识
---@param status string 请求行或状态行（已含 \r\n）
---@param block string? 预渲染好的头部块（每行已含 \r\n），原样拼在状态行后、不做校验，见 _response_lua
---@param headers table<string,string|number>? 附加头部 key→value 表；本函数按 info 类型自动生成的头
---       （Content-Length、Transfer-Encoding，以及 info 为 table 时的 Content-Type）
---       不得传入，传了整条丢弃并告警——留着会造出两条 Content-Length 或 TE 叠 CL，
---       严格实现与 smuggling 防御代理会拒收。info 非 table 时 Content-Type 正是经此传入。
---       值只收 string 与 number（整数值的浮点按整数写），其余类型同样整条丢弃并告警
---@param ckfunc fun(fin:boolean, data:lightuserdata|nil, size:integer)? chunked 接收回调
---@param info string|table<any,any>|fun(...):string?|nil 报文体；string 直接发送，table 自动 JSON 编码，function 流式分块（返回 nil 或空串终止流）
---       function 形态下**不得在回调内挂起**（不要 syn_send / sleep / 等任何消息）：chunked 各块之间
---       让出控制权，别的协程往同一 fd 上发的数据就插进本次报文体中间，对端解析必错。
---       这里没有连接级锁可加——只拿到 fd/skid，不像 pgsql copy_in 那样手里有 ctx 的 serial
---@param ... any 传给 info 函数的额外参数
---@return HttpPack|nil pack 解包后的响应表；rsp=true 或失败时返回 nil
local function _http_msg(rsp, nocl, headonly, sk, status, block, headers, ckfunc, info, ...)
    local msgtype = type(info)
    -- 有附加头才建表按段拼；没有（最常见）各分支直接 .. 成整条，不建表也不再多拼一次帧长头
    local msg, n
    if nil ~= headers then
        -- 预留 8 个数组槽（状态行 + 头部块 / 一个头 4 段 + 帧长头 + body），免得边写边扩。
        -- 槽里预置的是 nil，# 不可靠，全程用 n 计数，拼接时按 1..n 取
        msg = { status, block, nil, nil, nil, nil, nil, nil }
        n = (nil == block) and 1 or 2
        local autoflags = _auto_flags(msgtype, rsp)
        local sval, rc
        for key, val in pairs(headers) do
            sval, rc = head_check(key, val, autoflags)
            if nil == sval then
                _head_warn(rc, key)
            else
                msg[n + 1] = key
                msg[n + 2] = ": "
                msg[n + 3] = sval
                msg[n + 4] = "\r\n"
                n = n + 4
            end
        end
    end
    if "string" == msgtype then
        if nil == msg then
            return _http_send(rsp, sk, status .. (block or "") .. "Content-Length: " .. #info .. "\r\n\r\n"
                                       .. (headonly and "" or info), ckfunc)
        end
        n = n + 1
        msg[n] = "Content-Length: " .. #info .. "\r\n\r\n"
        if not headonly then
            n = n + 1
            msg[n] = info
        end
        return _http_send(rsp, sk, table.concat(msg, "", 1, n), ckfunc)
    elseif "table" == msgtype then
        local jmsg = json.encode(info)
        if nil == msg then
            return _http_send(rsp, sk, status .. (block or "") .. "Content-Type: application/json\r\nContent-Length: "
                                       .. #jmsg .. "\r\n\r\n" .. (headonly and "" or jmsg), ckfunc)
        end
        n = n + 1
        msg[n] = "Content-Type: application/json\r\nContent-Length: " .. #jmsg .. "\r\n\r\n"
        if not headonly then
            n = n + 1
            msg[n] = jmsg
        end
        return _http_send(rsp, sk, table.concat(msg, "", 1, n), ckfunc)
    elseif "function" == msgtype then
        -- 流式分块发送：每次调用 info(...) 取一块数据，拼成 chunked 格式后发送，
        -- info 返回 nil 或空串时结束流并补发终止块。头部先拼成串，随首块一起发；之后每块只发块本身
        local prefix
        if nil == msg then
            prefix = status .. (block or "") .. "Transfer-Encoding: chunked\r\n\r\n"
        else
            n = n + 1
            msg[n] = "Transfer-Encoding: chunked\r\n\r\n"
            prefix = table.concat(msg, "", 1, n)
        end
        if headonly then
            -- 生产者要跑完才知道长度, HEAD 本就不该真去跑它: 头到此为止, CL 省掉
            WARN("http: HEAD response body type 'function' has no known length, Content-Length omitted.")
            return _http_send(rsp, sk, prefix, ckfunc)
        end
        -- 每块一次 .. 拼接，不再过表
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
                -- 头部未发送时(首次迭代)由 prefix 一并带上,避免对端收到孤立的终止块
                -- 补了终止块后这仍是一条语法完整的 chunked 请求,对端必回响应,故走正常发送路径把它收完再丢弃:
                -- 响应自身也可能是 chunked(只 syn_send 收不干净),而 _wait_msg 按 skid 匹配、不区分请求,
                -- 漏收就会落到下一次请求注册的等待者上,连接从此错位一格。ckfunc 传 nil:不拿要丢弃的数据回调业务
                _http_send(rsp, sk, prefix .. "0\r\n\r\n", nil)
                return
            end
            -- 空串按结束处理：既是 chunked 终止块的语义，也避免此处零状态推进死循环
            if 0 == #rtn then
                break
            end
            smsg = prefix .. string.format("%x\r\n", #rtn) .. rtn .. "\r\n"
            if not srey.send(sk, smsg, #smsg, 1) then
                -- 中间帧失败仍尝试补发终止块,让对端退出 chunked 累积状态
                srey.send(sk, "0\r\n\r\n", 5, 1)
                return
            end
            prefix = ""
        end
        return _http_send(rsp, sk, prefix .. "0\r\n\r\n", ckfunc)
    else
        -- info 给了却不是 string/table/function：三个 body 分支一个都不命中，会被当成"无 body"
        -- 悄悄发走。数字、布尔、userdata 都落在这里，调用方从返回值上看不出 body 没发出去。
        -- 告警排在下面分帧之前：两种无 body 形态都要报，按响应/请求分开写会漏掉响应那半边
        if nil ~= info then
            WARN("http: unsupported body type '%s', sent without body.", msgtype)
        end
        local tail
        if rsp and not nocl then
            -- 响应无 body 必须显式 Content-Length: 0（RFC 7230 §3.3.3 规则 7：响应缺 CL/TE 时
            -- body 由连接关闭界定，keep-alive 下合规客户端会一直读到关闭才认为响应结束）。
            -- C 侧 http_pack_content 对空 body 也是统一写 Content-Length: 0
            tail = "Content-Length: 0\r\n\r\n"
        else
            -- 请求无 body 不带 CL/TE 即可（同规则 6）；1xx/204/304 响应则是禁止带（见 http.response）
            tail = "\r\n"
        end
        if nil == msg then
            return _http_send(rsp, sk, status .. (block or "") .. tail, ckfunc)
        end
        n = n + 1
        msg[n] = tail
        return _http_send(rsp, sk, table.concat(msg, "", 1, n), ckfunc)
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
    return method .. " " .. url .. REQ_TAIL
end

-- ── 公共 API ──────────────────────────────────────────────────────────────

---同步 GET 请求
---
---与同一连接上的其他请求并发时，须用 srey.serial 把整个调用圈进临界区：分片响应
---（chunked / 1xx / 既无 Content-Length 又无 Transfer-Encoding）的后续分片会被排在前面的协程
---拿走，双方各得半截；非分片响应不受影响。口径同 C 侧 coro_slice
---@param sk userdata 连接标识
---@param url string? URL 路径，默认 "/"；含 NUL/CRLF 时整条请求被拒（HTTP 请求拆分）
---@param headers table<string,string|number>? 附加头部
---@param ckfunc fun(fin:boolean, data:lightuserdata|nil, size:integer)? chunked 接收回调
---@return HttpPack|nil pack 解包后的响应表；失败返回 nil（超时/分片中断时响应可能未收完，
---此连接不应继续复用，应关闭——等待按 skid 匹配，残留响应会被下一次请求错认）。
---url 非法时同样返回 nil，但一个字节都没发出，连接可继续复用
function http.get(sk, url, headers, ckfunc)
    local status = _req_status("GET", url)
    if not status then
        return nil
    end
    return _http_msg(false, false, false, sk, status, nil, headers, ckfunc)
end

---同步 HEAD 请求：只要响应头，服务端不回报文体。
---解包侧拿不到请求方法，故发送前须先把 "HEAD" 登记到连接上（本函数已代劳）；漏登记的后果是
---带 Content-Length 的 HEAD 响应被当成有报文体，keep-alive 上会把下一条响应吃掉。
---
---登记挂在连接上而不是这一次请求上，故并发时同样须 srey.serial 圈住（理由见 http.get），
---否则那个登记会落到别人的响应上，把它的 body 当作不存在
---@param sk userdata 连接标识
---@param url string? URL 路径，默认 "/"；含 NUL/CRLF 时整条请求被拒（HTTP 请求拆分）
---@param headers table<string,string|number>? 附加头部
---@return HttpPack|nil pack 解包后的响应表（无报文体，data 为空）；失败返回 nil。
---url 非法或登记失败时一个字节都没发出，连接可继续复用
---
---名字不叫 head：本模块 40 行已把 srey_http.head（按 key 取单个响应头）导出为 http.head，
---同名会静默把那个访问器覆盖掉
function http.head_req(sk, url, headers)
    local status = _req_status("HEAD", url)
    if not status then
        return nil
    end
    -- 登记排在组请求之前：那时 method 就在手上，也不会出现"发出去了才想起登记"。
    -- 同一 fd 的命令走同一条 FIFO 队列，故它必然先于随后的发送生效
    if not core.http_set_method(sk, "HEAD") then
        WARN("http head: set method failed, skid %s.", tostring(sk.skid))
        return nil
    end
    return _http_msg(false, false, false, sk, status, nil, headers, nil)
end

---同步 POST 请求；info 为报文体（string/table/function），用法同 _http_msg
---
---并发约束同 http.get
---@param sk userdata 连接标识
---@param url string? URL 路径，默认 "/"；含 NUL/CRLF 时整条请求被拒（HTTP 请求拆分）
---@param headers table<string,string|number>? 附加头部
---@param ckfunc fun(fin:boolean, data:lightuserdata|nil, size:integer)? chunked 接收回调
---@param info string|table<any,any>|fun(...):string?|nil 报文体；string 直接发送，table 自动 JSON 编码，function 流式分块（返回 nil 或空串终止流）
---       function 形态下**不得在回调内挂起**（不要 syn_send / sleep / 等任何消息）：chunked 各块之间
---       让出控制权，别的协程往同一 fd 上发的数据就插进本次报文体中间，对端解析必错。
---       这里没有连接级锁可加——只拿到 fd/skid，不像 pgsql copy_in 那样手里有 ctx 的 serial
---@param ... any 传给 info 函数的额外参数
---@return HttpPack|nil pack 解包后的响应表；失败返回 nil（超时/分片中断时响应可能未收完，
---此连接不应继续复用，应关闭——等待按 skid 匹配，残留响应会被下一次请求错认）。
---url 非法时同样返回 nil，但一个字节都没发出，连接可继续复用
function http.post(sk, url, headers, ckfunc, info, ...)
    local status = _req_status("POST", url)
    if not status then
        return nil
    end
    return _http_msg(false, false, false, sk, status, nil, headers, ckfunc, info, ...)
end

local function _status_line_new(code)
    local status = string.format("HTTP/%s %03d %s\r\n", HTTP_VERSION, code, http.code_status(code))
    if "integer" == math.type(code) and code >= 100 and code <= 999 then
        _status_lines[code] = status
    end
    return status
end
---http.response 与 http.response_head 在 C 组包不接时走的 Lua 组包原路，也以 http._response_lua 导出给
---router（router 先自己调 http._respond，返回 false 才调它，省一层转发）。
---业务不直接调本函数;fd 之后各参数的完整约束见 http.response
---@param headonly boolean 回 HEAD 请求：头与同一资源的 GET 逐字节一致，但不发报文体
---@param sk userdata 连接标识
---@param code integer 状态码（如 200、404）
---@param block string? 预渲染好的头部块（每行已含 \r\n），原样拼在状态行后、不做校验：只给模块常量用，
---       且不得含本模块按 info 自动生成的头（帧长头；info 为 table 时还有 Content-Type）
---@param headers table<string,string|number>? 附加头部
---@param info string|table<any,any>|fun(...):string?|nil 报文体，约束见 http.response
---@param ... any 传给 info 函数的额外参数
local function _response_lua(headonly, sk, code, block, headers, info, ...)
    local status = _status_lines[code]
    if nil == status then
        status = _status_line_new(code)
    end
    -- RFC 7230 §3.3.2：1xx 与 204 一律禁止带 Content-Length；304 允许带，但那个值应当反映
    -- 实体的真实长度，这里根本没有实体，补 Content-Length: 0 等于谎报资源为空，故一并跳过。
    -- 严格代理会因此丢弃或重置这类响应，所以不能对所有无 body 响应无差别补 CL
    local nocl = code < 200 or 204 == code or 304 == code
    if nocl then
        info = nil-- 这三类响应同样禁带报文体：给了也丢，与 C 侧 _router_send_core 同口径
    end
    _http_msg(true, nocl, headonly, sk, status, block, headers, nil, info, ...)
end
-- http.response 与 http.response_head 的共同实现，参数同 _response_lua
local function _response(headonly, sk, code, block, headers, info, ...)
    -- 常见形状（string / nil 报文体、没有头表时的 table 报文体、合法头表）整条在 C 里组包直发，字节同 _response_lua 的原路；
    -- 返回 false 才往下走（带头表的 table、function 报文体、要告警丢头、怪码、编不成 JSON 等）
    if http_respond(sk, code, headers, info, headonly, block) then
        return
    end
    _response_lua(headonly, sk, code, block, headers, info, ...)
end
-- 给 router 的两半：先 _respond（C 组包直发，参数见 srey.http.respond），返回 false 再 _response_lua
http._respond = http_respond
http._response_lua = _response_lua
---向客户端发送 HTTP 响应（单向，不等待回包）
---@param sk userdata 连接标识
---@param code integer 状态码（如 200、404）
---@param headers table<string,string|number>? 附加头部
---@param info string|table<any,any>|fun(...):string?|nil 报文体；string 直接发送，table 自动 JSON 编码，function 流式分块（返回 nil 或空串终止流）
---       function 形态下**不得在回调内挂起**（不要 syn_send / sleep / 等任何消息）：chunked 各块之间
---       让出控制权，别的协程往同一 fd 上发的数据就插进本次报文体中间，对端解析必错。
---       这里没有连接级锁可加——只拿到 fd/skid，不像 pgsql copy_in 那样手里有 ctx 的 serial
---@param ... any 传给 info 函数的额外参数
function http.response(sk, code, headers, info, ...)
    _response(false, sk, code, nil, headers, info, ...)
end

---回 HEAD 请求：头与同一资源的 GET 完全一致（含按 info 算出的真实 Content-Length），但不发
---报文体。info 照常传 GET 会返回的那份，本函数只用它算长度不写出去。
---解包侧拿不到请求方法，故"这是不是 HEAD 请求"只有调用方知道——router 走 ctx.method 分流；
---自己写 _net_recv 的业务须自行判断，用错会让对端把多发的字节当成下一条响应，keep-alive 错位
---@param sk userdata 连接标识
---@param code integer 状态码
---@param headers table<string,string|number>? 附加头部
---@param info string|table<any,any>|fun(...):string?|nil 若是 GET 会返回的报文体；function 形态算不出
---       长度，只发头且省略 Content-Length
---@param ... any 传给 info 函数的额外参数
function http.response_head(sk, code, headers, info, ...)
    _response(true, sk, code, nil, headers, info, ...)
end

return http
