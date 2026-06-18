-- 手工维护的共享类型定义，供 meta/ 下各生成文件引用。
-- 本文件不由 tools/gen_meta.py 生成，修改不会被覆盖。

---@class WebSocketFrame
---@field fin     boolean              帧是否完整（fin bit = 1）
---@field prot    integer              WebSocket opcode（text/binary/ping/pong/close/continuation）
---@field secprot string?              Sec-WebSocket-Protocol；仅握手帧存在
---@field secpack lightuserdata?       Sec-WebSocket-Accept 验证数据指针；仅握手帧存在
---@field data    lightuserdata?       帧载荷数据指针；空帧为 nil
---@field size    integer              载荷字节数；空帧为 0

---@class ParsedURL
---@field scheme string               协议（如 "http"、"https"）
---@field user   string?              用户名；URL 中无用户信息时为 nil
---@field psw    string?              密码；URL 中无密码时为 nil
---@field host   string               主机名或 IP
---@field port   string?              端口号字符串（如 "8080"）；URL 中无端口时为 nil
---@field path   string?              重组后的路径（如 "/user/42"）；语义随 decode 参数：decode=true 已解码，decode=false 保留原始编码；无路径段时为 nil
---@field query  string?              重组后的查询字符串（如 "k=v&k2=v2"）；语义随 decode 参数；无查询参数时为 nil
---@field segs   string[]             路径段数组（如 {"user","42"}）；语义随 decode 参数；router 直接消费, %2F 不当分隔符
---@field anchor string?              片段标识符（# 后部分）；不存在时为 nil
---@field param  table<string,string> 查询字符串键值对；语义随 decode 参数；无查询字符串时为空表

---@class RedisAggValue
---@field resp_type  "array"|"set"|"map"|"push"|"attr"  聚合类型名
---@field resp_nelem integer  元素计数；map/attr 实际字段数为此值 × 2；-1 表示 null 聚合

---@class TaskStatItem
---@field nmsg            integer  累计消息条数
---@field dispatch_cpu_ns integer  累计 dispatch 占用线程 CPU 纳秒（不含 IO 等待 / 被抢占）

---@class TaskStat
---@field total   TaskStatItem                    各 mtype 桶之和
---@field by_type table<integer, TaskStatItem>    mtype 整数 → 桶；仅包含至少处理过 1 条消息的 mtype

---@class _bson_binary  BSON Binary 包装 userdata（lualib/lbind/lbson.c MT_BSON_BINARY，opaque）
---@class _bson_date    BSON Date 包装 userdata（lualib/lbind/lbson.c MT_BSON_DATE，opaque）
---@class _bson_int64   BSON INT64 包装 userdata（lualib/lbind/lbson.c MT_BSON_INT64，opaque）
---@class _bson_oid     BSON OID 包装 userdata（lualib/lbind/lbson.c MT_BSON_OID，opaque）
