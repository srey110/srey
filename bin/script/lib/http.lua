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
---@field fin     boolean?                chunked 模式下是否已接收完所有分片；非 chunked 时不存在

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
        pack.fin = false
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
                pack.fin = true
                break
            end
        end
        if chunks and #chunks > 0 then
            pack.data = table.concat(chunks)
        end
    end
    return pack
end

---构造并发送 HTTP 消息的核心函数。info 支持 string（带 Content-Length）、
---table（自动 JSON 编码）、function（chunked 流式分块发送，返回 nil 终止流）三种类型
---@param rsp boolean 是否为响应（true=单向，false=同步请求）
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param status string 请求行或状态行（已含 \r\n）
---@param headers table<string,any>? 附加头部 key→value 表；
---       禁止包含 Content-Length / Content-Type / Transfer-Encoding 等由本函数
---       根据 info 类型自动添加的头，否则触发重复头部部分严格 HTTP 实现 / smuggling 防御代理会拒收
---@param ckfunc fun(fin:boolean, data:lightuserdata|nil, size:integer)? chunked 接收回调
---@param info string|table|fun(...):string?|nil 报文体；string 直接发送，table 自动 JSON 编码，function 流式分块（返回 nil 终止流）
---@param ... any 传给 info 函数的额外参数
---@return HttpPack|nil pack 解包后的响应表；rsp=true 或失败时返回 nil
local function _http_msg(rsp, fd, skid, status, headers, ckfunc, info, ...)
    local msg = {}
    table.insert(msg, status)
    if nil ~= headers then
        for key, val in pairs(headers) do
            local n = #msg
            msg[n + 1] = key
            msg[n + 2] = ": "
            msg[n + 3] = tostring(val)
            msg[n + 4] = "\r\n"
        end
    end
    local msgtype = type(info)
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
        -- 直到 info 返回 nil 时补发终止块。
        table.insert(msg, "Transfer-Encoding: chunked\r\n\r\n")
        local smsg, rtn
        while true do
            rtn = info(...)
            if rtn then
                if "string" ~= type(rtn) then
                    ERROR("chunked function must return string, got %s.", type(rtn))
                    table.insert(msg, "0\r\n\r\n")
                    return _http_send(rsp, fd, skid, msg, ckfunc)
                end
                if #rtn > 0 then
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
            else
                table.insert(msg, "0\r\n\r\n")
                return _http_send(rsp, fd, skid, msg, ckfunc)
            end
        end
    else
        -- 无报文体，仅发送空行结束头部
        table.insert(msg, "\r\n")
        return _http_send(rsp, fd, skid, msg, ckfunc)
    end
end

-- ── 公共 API ──────────────────────────────────────────────────────────────

---同步 GET 请求
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param url string? URL 路径，默认 "/"
---@param headers table<string,any>? 附加头部
---@param ckfunc fun(fin:boolean, data:lightuserdata|nil, size:integer)? chunked 接收回调
---@return HttpPack|nil pack 解包后的响应表；失败返回 nil
function http.get(fd, skid, url, headers, ckfunc)
    local status = string.format("GET %s HTTP/%s\r\n", url or "/", HTTP_VERSION)
    return _http_msg(false, fd, skid, status, headers, ckfunc)
end

---同步 POST 请求；info 为报文体（string/table/function），用法同 _http_msg
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param url string? URL 路径，默认 "/"
---@param headers table<string,any>? 附加头部
---@param ckfunc fun(fin:boolean, data:lightuserdata|nil, size:integer)? chunked 接收回调
---@param info string|table|fun(...):string?|nil 报文体；string 直接发送，table 自动 JSON 编码，function 流式分块（返回 nil 终止流）
---@param ... any 传给 info 函数的额外参数
---@return HttpPack|nil pack 解包后的响应表；失败返回 nil
function http.post(fd, skid, url, headers, ckfunc, info, ...)
    local status = string.format("POST %s HTTP/%s\r\n", url or "/", HTTP_VERSION)
    return _http_msg(false, fd, skid, status, headers, ckfunc, info, ...)
end

---向客户端发送 HTTP 响应（单向，不等待回包）
---@param fd integer socket fd
---@param skid integer 连接 skid
---@param code integer 状态码（如 200、404）
---@param headers table<string,any>? 附加头部
---@param info string|table|fun(...):string?|nil 报文体；string 直接发送，table 自动 JSON 编码，function 流式分块（返回 nil 终止流）
---@param ... any 传给 info 函数的额外参数
function http.response(fd, skid, code, headers, info, ...)
    local status = string.format("HTTP/%s %03d %s\r\n", HTTP_VERSION, code, http.code_status(code))
    _http_msg(true, fd, skid, status, headers, nil, info, ...)
end

return http
