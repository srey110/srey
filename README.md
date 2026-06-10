# Srey

轻量级 C / Lua 高性能异步服务框架。  
基于协程 + 事件驱动，内置多协议支持，跨平台，可选 SSL 与 Lua 脚本扩展。

---

## 特性

- **事件模型**：IOCP（Windows）、EPOLL（Linux）、KQUEUE（macOS/BSD）、EVPORT（Solaris）、POLLSET（AIX）
- **协程**：同步风格编写异步逻辑，无回调地狱
- **多协议**：HTTP、WebSocket、MQTT、DNS、MySQL、PostgreSQL、Redis、MongoDB、SMTP、自定义协议
- **SSL/TLS**：基于 OpenSSL，支持 PEM / ASN1 / PKCS12 证书
- **Lua 脚本**：内嵌 Lua，支持热更新业务逻辑
- **跨平台**：Windows、Linux、macOS、FreeBSD、Solaris、AIX、HPUX

---

## 目录结构

```
srey/
├── lib/
│   ├── base/               OS 抽象、宏、内存、类型
│   ├── containers/         数组、队列、堆、哈希表、无锁队列
│   ├── crypt/              AES、DES、MD5、SHA、HMAC、Base64、CRC…
│   ├── event/              事件循环抽象 + SSL 封装
│   ├── protocol/           协议实现
│   │   ├── mongo/          MongoDB
│   │   ├── mqtt/           MQTT 3.1.1 / 5.0
│   │   ├── mysql/          MySQL
│   │   ├── pgsql/          PostgreSQL
│   │   └── smtp/           SMTP
│   ├── services/           Harbor 跨服通信
│   ├── srey/               Task 系统、调度器、协程
│   ├── thread/             互斥锁、读写锁、自旋锁、条件变量
│   └── utils/              日志、定时器、时间轮、Buffer、雪花 ID…
├── lualib/                 Lua 运行时 + C 绑定
├── srey/                   主程序入口
├── test/                   C 单元测试（CuTest）
├── bin/
│   ├── script/             Lua 脚本与测试用例
│   ├── configs/            运行时配置
│   └── keys/               SSL 证书
├── CMakeLists.txt
├── mk.sh
└── srey.sln                Visual Studio 解决方案
```

---

## 编译

### 依赖

| 依赖 | 必选 | 说明 |
|------|------|------|
| gcc / clang (C99) | ✓ | |
| pthread | ✓ | |
| OpenSSL | 可选 | `WITH_SSL=1` 时需要 |
| Lua     | 内置 | 源码已包含，无需安装 |

---

### mk.sh（Linux / macOS / Unix）

```sh
# Release
sh mk.sh

# Debug（ASan + UBSan）
DEBUG=1 sh mk.sh

# 编译测试套件
sh mk.sh test

# 清理
sh mk.sh clean
```

---

### CMake

```sh
# 配置（macOS 示例）
cmake -S . -B build \
    -DWITH_LUA=ON \
    -DWITH_SSL=ON \
    -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3

# 编译
cmake --build build

# Debug
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DWITH_LUA=ON -DWITH_SSL=ON
cmake --build build
```

**CMake 选项**

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `WITH_SSL` | `OFF` | 启用 SSL/TLS |
| `WITH_LUA` | `ON`  | 启用 Lua 脚本 |
| `CMAKE_BUILD_TYPE` | `Release` | `Debug` 开启 ASan/UBSan |

---

### Windows

用 Visual Studio 2015+ 打开 `srey.sln`，直接编译。

---

## 配置

编译前修改 `lib/base/config.h`：

| 宏 | 默认 | 说明 |
|----|------|------|
| `WITH_SSL` | `0` | 启用 OpenSSL |
| `WITH_LUA` | `1` | 启用 Lua |
| `MEMORY_CHECK` | `1` | 内存检测 |
| `KEEPALIVE_TIME` | `30` | TCP keepalive 时间（秒）|
| `MAX_PACK_SIZE` | `65536` | 最大包大小（字节）|
| `MAX_RECV_SIZE` | `4096` | 单次最大接收（字节）|

---

## 核心概念

### Task（任务）

Task 是框架的核心调度单元，每个服务运行在独立的 Task 中。

```c
// 创建并注册 Task
void my_service(loader_ctx *loader, name_t name) {
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, _closing);
}

// 启动回调：注册网络事件和监听
static void _startup(task_ctx *task) {
    on_recved(task, _net_recv);
    uint64_t id;
    task_listen(task, PACK_HTTP, NULL, "0.0.0.0", 8080, &id, 0);
}

// 接收回调
static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid,
    uint8_t pktype, uint8_t client, uint8_t slice,
    void *data, size_t size) {
    // 处理数据
}
```

**Task 间通信**

```c
// 单向调用（无需等待响应）
task_call(task, dst_name, data, size, copy);

// 请求/响应（阻塞等待，基于协程）
void *resp = NULL;
size_t resp_size = 0;
coro_request(task, dst_name, data, size, &resp, &resp_size, timeout_ms);
```

---

### 协程

使用协程将异步操作写成同步风格：

```c
// 发起连接并等待结果（不阻塞事件循环）
SOCKET fd;
uint64_t skid;
int32_t ret = coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", 80, &fd, &skid);

// 发送并等待响应
void *resp = NULL;
size_t resp_size = 0;
ret = coro_send(task, fd, skid, data, size, &resp, &resp_size);

// 非阻塞睡眠
coro_sleep(task, 1000); // 1000ms

// SSL 握手升级
coro_ssl_exchange(task, fd, skid, 1, evssl);
```

---

### SSL

```c
// 加载证书，创建 SSL 上下文
evssl_ctx *ssl = evssl_new(...);

// 监听时启用 SSL
task_listen(task, PACK_HTTP, ssl, "0.0.0.0", 443, &id, 0);

// 连接时启用 SSL
coro_connect(task, PACK_HTTP, ssl, "127.0.0.1", 443, &fd, &skid);
```

---

## 协议使用

### HTTP

```c
// 打包响应
binary_ctx bwriter;
binary_init(&bwriter, NULL, 0, 0);
http_pack_resp(&bwriter, 200);
http_pack_content(&bwriter, body, body_len);
ev_send(&task->loader->netev, fd, skid, bwriter.data, bwriter.offset, 0);
```

### MySQL

```c
mysql_ctx mysql;
mysql_init(&mysql, "127.0.0.1", 3306, NULL, "root", "password", "mydb", "utf8mb4");
// 查询结果通过 on_recved 回调返回
```

### PostgreSQL

```c
pgsql_ctx pg;
pgsql_init(&pg, "127.0.0.1", 5432, NULL, "postgres", "password", "mydb");
```

### Redis

```c
// RESP 协议通过自定义协议包收发
// 支持 RESP2 / RESP3
```

### SMTP

```c
smtp_ctx smtp;
smtp_init(&smtp, "smtp.example.com", 465, ssl, "user@example.com", "password");
```

---

## Lua 脚本

### HTTP 服务示例

```lua
local srey = require("lib.srey")
local http = require("lib.http")

srey.startup(function()
    srey.on_recved(function(pktype, fd, skid, client, slice, data, size)
        http.response(fd, skid, 200, nil, os.date("%Y-%m-%d %H:%M:%S"))
    end)
    srey.listen(PACK_TYPE.HTTP, 0, "0.0.0.0", 8080)
end)
```

在 `startup.lua` 中注册：
```lua
task.register("httpd", require("httpd"))
```

### WebSocket 服务示例

```lua
local srey  = require("lib.srey")
local websock = require("lib.websock")

srey.startup(function()
    srey.on_recved(function(pktype, fd, skid, client, slice, data, size)
        websock.send(fd, skid, data, size)   -- echo
    end)
    srey.listen(PACK_TYPE.WEBSOCK, 0, "0.0.0.0", 8081)
end)
```

### 定时器

```lua
srey.timeout(1000, function()
    print("1 second later")
end)
```

---

## Harbor（跨服通信）

Harbor 提供多服务器节点间的透明通信：

```c
// 向远端节点发起请求
harbor_call(task, remote_name, data, size, copy);
harbor_request(task, remote_name, data, size, &resp, &resp_size, timeout_ms);
```

---

## 运行测试

```sh
# 编译测试
sh mk.sh test

# 执行
./bin/test
```

