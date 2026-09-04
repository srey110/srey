-- 手工维护的共享类型定义，供 meta/ 下各生成文件引用。
-- 本文件不由 tools/gen_meta.py 生成，修改不会被覆盖。

---@class WebSocketFrame
---@field fin     integer              帧是否完整（fin bit = 1）；推的是整数不是布尔
---@field prot    integer              WebSocket opcode（text/binary/ping/pong/close/continuation）
---@field secprot integer?             协商出的子协议，取值同 PACK_TYPE（如 PACK_TYPE.MQTT）；
---                                    未协商子协议时该字段不存在
---@field secpack lightuserdata?       子协议包指针（如 WS 承载的 MQTT 包）；控制帧（PING/PONG/
---                                    CLOSE）与零长数据帧即使 secprot 非空也没有它，取用前必判
---@field data    lightuserdata        帧载荷数据指针，恒非 nil（指向包尾柔性数组）；
---                                    零长帧下 size 为 0，仍可连同 size 转手给 pack_text/pack_binary 一族
---@field size    integer              载荷字节数；空帧为 0

---@class MqttConnectInfo
---@field version     integer  协议级别（4 = 3.1.1，5 = 5.0）
---@field cleanstart  integer  clean session / clean start 标志
---@field keepalive   integer  保活秒数
---@field willflag    integer  是否携带遗嘱
---@field willqos     integer  遗嘱 QoS
---@field willretain  integer  遗嘱 retain 标志
---@field clientid    string   客户标识符
---@field user        string?  用户名；未携带时不存在
---@field password    string?  密码；未携带时不存在
---@field willtopic   string?  遗嘱主题；willflag 为 0 时不存在
---@field willpayload string?  遗嘱内容；同上

---@class MqttSubscribeItem
---@field topic  string   主题过滤器
---@field qos    integer  订阅 QoS
---@field nl     integer  No Local 标志（MQTT 5.0）
---@field rap    integer  Retain As Published 标志（MQTT 5.0）
---@field retain integer  Retain Handling（MQTT 5.0）

---@class RouterMatchedURL
---@field path  string                 规范化后的请求路径
---@field param table<string,string>   查询串键值表

---@class MemStat
---@field nalloc integer 累计分配次数
---@field nfree  integer 累计释放次数
---@field live   integer 当前活跃分配数

---@class TaskListItem
---@field name   string?  task 名；匿名 task 无此字段
---@field handle integer  task 句柄

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

---聚合节点的原始值：C 层直接吐出的表，带两个哨兵字段。只有 redis.value() 返回这个形态；
---redis.unpack 链路上的表都已被 _node 摘净哨兵，那个形态见 RedisAggPayload
---@class RedisAggValue
---@field resp_type  "array"|"set"|"map"|"push"|"attr"  聚合类型名
---@field resp_nelem integer  元素计数；map/attr 实际字段数为此值 × 2；-1 表示 null 聚合

---摘掉哨兵后的载荷表：array/set/push 是数组，map/attr 是键值对，元素即 Redis 数据本身，无固定字段。
---与 RedisAggValue 分开命名,是因为这个形态下 resp_type / resp_nelem 必然不存在
---@class RedisAggPayload

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
