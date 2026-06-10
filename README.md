# Srey

轻量级 C / Lua 高性能异步服务框架。  
基于协程 + 事件驱动，内置多协议支持，跨平台，可选 SSL 与 Lua 脚本扩展。

---

## 特性

- **事件模型**：IOCP（Windows）、EPOLL（Linux）、KQUEUE（macOS/BSD）、EVPORT（Solaris）、POLLSET（AIX）
- **协程**：同步风格编写异步逻辑
- **多协议**：HTTP、WebSocket、MQTT、DNS、MySQL、PostgreSQL、Redis、MongoDB、SMTP、自定义协议
- **SSL/TLS**：基于 OpenSSL，支持 PEM / ASN1 / PKCS12 证书
- **Lua 脚本**：内嵌 Lua，支持热更新业务逻辑
- **跨平台**：Windows、Linux、macOS、FreeBSD...

---

## 目录结构

```
srey/
├── lib/
│   ├── base/               OS 抽象、宏、内存、类型、编译期配置
│   ├── containers/         数组、队列、堆、哈希表、无锁队列
│   ├── crypt/              AES、DES、MD5、SHA、HMAC、Base64、CRC…
│   ├── event/              事件循环抽象 + SSL 封装（evssl）
│   ├── path/               路径树（path_get/path_insert）
│   ├── protocol/           协议实现
│   │   ├── mongo/          MongoDB
│   │   ├── mqtt/           MQTT 3.1.1 / 5.0
│   │   ├── mysql/          MySQL
│   │   ├── pgsql/          PostgreSQL
│   │   └── smtp/           SMTP
│   ├── serial/             序列化(bson seri)
│   ├── services/           Harbor 跨服通信、DataCenter、SubCenter、调试控制台
│   ├── srey/               Task 系统、调度器、协程
│   ├── thread/             互斥锁、读写锁、自旋锁、条件变量
│   └── utils/              日志、定时器、时间轮、Buffer、雪花 ID…
├── lualib/                 Lua 运行时 + C 绑定
├── srey/                   主程序入口（main.c）
├── test/                   C 单元测试（CuTest）+ 集成测试
├── bin/
│   ├── configs/            运行时配置（config.json）
│   ├── html/               调试控制台页面
│   ├── keys/               SSL 证书
│   ├── py_assist/          Python 辅助脚本
│   └── script/             Lua 脚本与测试用例
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
| OpenSSL | 可选 | `WITH_SSL=1` 时需要 |

---

### mk.sh（Linux / macOS / Unix）

```sh
# Release（默认编译 srey）
sh mk.sh

# 编译 srey + test
sh mk.sh all

# 仅编译测试套件
sh mk.sh test

# Debug（-O0 -g3）
sh mk.sh debug

# Debug + ASan/UBSan（macOS ARM64 须与 debug 同用）
sh mk.sh asan debug

# ThreadSanitizer（与 asan 互斥）
sh mk.sh tsan

# 清理构建产物
sh mk.sh clean
```

参数顺序：第 1 个为构建目标（空 = `srey` / `all` / `test` / `clean`），其后为可组合的 flags：`debug`、`asan`、`tsan`、`m32`、`m64`。

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

### 编译期配置（`lib/base/config.h`）

| 宏 | 默认 | 说明 |
|----|------|------|
| `WITH_SSL` | `1` | 启用 OpenSSL |
| `WITH_LUA` | `1` | 启用 Lua |
| `MEMORY_CHECK` | `1` | 内存检测 |
| `KEEPALIVE_TIME` | `30` | TCP keepalive 时间（秒）|
| ... | | |

> `mk.sh` 会读取 `config.h` 的 `WITH_SSL` / `WITH_LUA` 决定是否链接 OpenSSL 与编译 lualib。

### 运行期配置（`bin/configs/config.json`）

关键字段：`nnet` / `nworker`（线程数，0 = CPU 核数）、`loglv`（日志级别）、`stacksize`（协程栈字节数）、`dns`、`script`（Lua 脚本目录）；`harbor` / `datacenter` / `subcenter` 为嵌套对象，例如 `harbor`: `{ name, ssl, ip, port, key }`。

---

## 核心概念

### Task（任务）

Task 是框架的核心调度单元，每个服务运行在独立的 worker 中。

```c
// 创建并注册 Task（name 为字符串任务名，quecap=0 用默认队列容量）
void my_service(loader_ctx *loader, const char *name) {
    task_ctx *task = task_new(loader, name, 0, NULL, NULL, NULL);
    if (ERR_OK != task_register(task, _startup, _closing)) {
        task_free(task);
    }
}

// 启动回调：注册接收回调和监听
static void _startup(task_ctx *task) {
    task_recved(task, _net_recv);
    uint64_t id;
    if (ERR_OK != task_listen(task, PACK_HTTP, NULL, "0.0.0.0", 8080, &id, 0)) {
        LOG_WARN("listen error");
    }
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
// 单向请求（不等待响应）；dst 为目标 task 指针，reqtype 标识请求类型
task_call(dst, reqtype, data, size, copy);

// 请求/响应（协程内阻塞等待）；dst 为目标 task，src 为当前 task
// 返回响应数据，仅在下次 yield（再调任意 coro_* API）前有效，需要保留请自行拷贝
int32_t erro = 0;
size_t resp_size = 0;
void *resp = coro_request(dst, task, reqtype, data, size, copy, &erro, &resp_size);
```

---

### 协程

使用协程将异步操作写成同步风格：

```c
// 发起连接并等待结果（不阻塞事件循环）
// 参数：task, pktype, evssl, ip, port, netev, extra, &fd, &skid
SOCKET fd;
uint64_t skid;
int32_t ret = coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", 80, 0, NULL, &fd, &skid);

// 发送并等待响应；返回响应数据（下次 yield 前有效，需保留请拷贝）
size_t resp_size = 0;
void *resp = coro_send(task, fd, skid, data, size, &resp_size, 0);

// 非阻塞睡眠
coro_sleep(task, 1000); // 1000ms

// SSL 握手升级（client=1 作为客户端）
coro_ssl_exchange(task, fd, skid, 1, evssl);
```

---

### SSL

```c
// 加载证书创建 SSL 上下文（ca, cert, key, 证书类型）
evssl_ctx *ssl = evssl_new("ca.crt", "server.crt", "server.key", SSL_FILETYPE_PEM);

// 监听时启用 SSL
task_listen(task, PACK_HTTP, ssl, "0.0.0.0", 443, &id, 0);

// 连接时启用 SSL
coro_connect(task, PACK_HTTP, ssl, "127.0.0.1", 443, 0, NULL, &fd, &skid);
```

---

## 协议使用

### HTTP

```c
// 打包响应（复用 binary_ctx）
binary_ctx bwriter;
binary_init(&bwriter, NULL, 0, 0);
http_pack_resp(&bwriter, 200);
http_pack_content(&bwriter, body, body_len);
ev_send(&task->loader->netev, fd, skid, bwriter.data, bwriter.offset, 0);
```

### MySQL

```c
// 参数：mysql, ip, port, evssl, user, password, database, charset, maxpk
mysql_ctx mysql;
mysql_init(&mysql, "127.0.0.1", 3306, NULL, "root", "password", "mydb", "utf8mb4", 0);
// 查询结果通过接收回调返回
```

### PostgreSQL

```c
pgsql_ctx pg;
pgsql_init(&pg, "127.0.0.1", 5432, NULL, "postgres", "password", "mydb");
```

### Redis

```c
// 监听/连接时使用 PACK_REDIS（Lua 层 PACK_TYPE.REDIS），支持 RESP2 / RESP3
// Lua 层封装见 bin/script/lib/redis.lua
```

### MongoDB

```c
// 使用 PACK_MONGO（Lua 层 PACK_TYPE.MGDB）；Lua 层封装见 bin/script/lib/mongo.lua
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
        http.response(fd, skid, 200, nil, "hello srey")
    end)
    if ERR_FAILED == srey.listen(PACK_TYPE.HTTP, SSL_NAME.NONE, "0.0.0.0", 8080) then
        WARN("listen error")
    end
end)
```

### WebSocket 服务示例

服务端用 `srey.websock`（C 绑定层）打包帧，配合 `srey.send` 发送：

```lua
local srey    = require("lib.srey")
local websock = require("srey.websock")

srey.startup(function()
    srey.on_recved(function(pktype, fd, skid, client, slice, data, size)
        local pack = websock.unpack(data)
        if 0x01 == pack.prot then       -- TEXT，回显
            local frame, fsize = websock.pack_text(0, 1, pack.data, pack.size)
            srey.send(fd, skid, frame, fsize, 0)
        elseif 0x09 == pack.prot then   -- PING → PONG
            local frame, fsize = websock.pack_pong(0)
            srey.send(fd, skid, frame, fsize, 0)
        end
    end)
    srey.listen(PACK_TYPE.WEBSOCK, SSL_NAME.NONE, "0.0.0.0", 8081)
end)
```

### 定时器

```lua
srey.timeout(1000, function()
    print("1 second later")
end)
```

### 注册服务

在启动脚本（`bin/script/startup.lua` 引导的注册流程）中用 `task.register` 注册 Lua task：

```lua
local task = require("srey.task")

-- task.register(脚本文件名, task名, 队列容量, ...额外参数)
-- 脚本文件名支持 a.b 形式映射到 script/a/b.lua；队列容量 0 用默认
task.register("httpd", "httpd", 0)
```

---

## Harbor（跨服通信）

Harbor 提供多服务器节点间的透明通信。C 层用 `harbor_start` 启动节点，`harbor_pack` 打包跨服请求：

```c
// 启动 harbor 节点（节点名、SSL 名、监听地址、密钥）
harbor_start(loader, "node1", NULL, "0.0.0.0", 6789, "secret");
```

Lua 层经由 harbor 连接向远端 task 发起调用 / 请求：

```lua
srey.net_call(fd, skid, dst, reqtype, key, data, size)
local resp = srey.net_request(fd, skid, dst, reqtype, key, data, size)
```

> 节点名、监听地址与密钥也可在 `bin/configs/config.json` 的 `harbor` 块（`name` / `ip` / `port` / `ssl` / `key`）中配置，由 `main.c` 启动时读取。

---

## 更多用法参考代码

## 运行测试

```sh
# 编译测试
sh mk.sh test

# 执行（跑完阻塞等待 SIGINT 收尾）
./bin/test
```

集成测试（`test_srey`）会拉起完整 loader + 网络事件循环；纯内存单元测试覆盖 base / containers / crypt / utils / thread / protocol。
