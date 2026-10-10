# Srey

轻量级 C / Lua 高性能异步服务框架。  
基于协程 + 事件驱动，内置多协议支持，跨平台，可选 SSL 与 Lua 脚本扩展。

---

## 特性

- **事件模型**：IOCP（Windows）、epoll（Linux）、kqueue（macOS / BSD）、evport（Solaris）、pollset（AIX）、/dev/poll（HP-UX）
- **调度**：task 消息驱动，默认跑在 worker 线程池；task 也可绑到 net 线程上执行，多核监听可按线程分组
- **协程**：同步风格编写异步逻辑（C 侧基于 minicoro，Lua 侧用原生协程）
- **多协议**：HTTP、WebSocket、MQTT 3.1.1 / 5.0、DNS、Redis、MySQL、PostgreSQL、MongoDB、SMTP、KCP、自定义协议
- **SSL/TLS**：基于 OpenSSL，支持 PEM / ASN1 / PKCS12 证书，支持 TLS 1.3 KeyUpdate
- **Lua 脚本**：内嵌 Lua 5.5，支持热更新与调试控制台注入
- **上层组件**：HTTP 路由、Harbor 跨服通信、HTTP 调试控制台
- **跨平台**：Windows、Linux、macOS、FreeBSD...

---

## 目录结构

```
srey/
├── lib/
│   ├── advance/            上层组件：HTTP 路由器、Harbor 跨服通信、调试控制台
│   ├── base/               OS 抽象、通用宏、内存分配、字节序与数字转换、编译期配置
│   ├── containers/         纯头文件宏容器：数组、队列、堆、哈希表、红黑树、无锁队列
│   ├── coro/               通用协程调度器（基于 minicoro）
│   ├── crypt/              AES、DES、MD5、SHA、HMAC、SCRAM、Base64、CRC、xxHash…
│   ├── event/              事件循环 + SSL 封装（evssl）
│   ├── protocol/           协议实现
│   │   ├── kcp/            KCP 可靠 UDP
│   │   ├── mongo/          MongoDB
│   │   ├── mqtt/           MQTT 3.1.1 / 5.0
│   │   ├── mysql/          MySQL
│   │   ├── pgsql/          PostgreSQL
│   │   └── smtp/           SMTP
│   ├── serial/             序列化：BSON、seri、JSON（yyjson）
│   ├── srey/               task 系统、调度器、协程 task 与常用协程封装
│   ├── thread/             互斥锁、读写锁、自旋锁、条件变量
│   └── utils/              日志、定时器、时间轮、Buffer、雪花 ID、UUID…
├── lualib/                 Lua 5.5 运行时 + C 绑定（lbind）
├── srey/                   主程序入口（main.c）与服务装配（startup.c）
├── test/                   C 单元测试（CuTest）+ 集成测试
├── bin/
│   ├── configs/            运行时配置（config.json）
│   ├── html/               调试控制台页面
│   ├── keys/               SSL 证书（create.sh 生成）
│   ├── py_assist/          Python 端到端测试脚本
│   └── script/             Lua 脚本与测试用例
├── tools/                  deps.py（第三方依赖）、gen_meta.py（Lua 类型存根）等
├── docker-compose.yml      集成测试用的后端服务
├── mk.sh                   Unix 构建脚本
├── CMakeLists.txt          Unix CMake 构建（与 mk.sh 同口径）
├── mk.bat                  Windows 构建脚本
└── srey.sln                Visual Studio 解决方案
```

---

## 编译

### 依赖

| 依赖 | 必选 | 说明 |
|------|------|------|
| gcc / clang（C99） | ✓ | Windows 用 VS |
| Python 3 | ✓ | 跑 `tools/deps.py` 准备 OpenSSL / mimalloc |
| perl | `WITH_SSL=1` | 编 OpenSSL |
| cmake | `WITH_MIMALLOC=1` | 编 mimalloc；用 CMake 构建本项目时也要 |

OpenSSL 与 mimalloc 都由 `deps.py` 编成静态库，不依赖系统里装的版本。

---

### mk.sh（Linux / macOS / Unix）

```sh
# Release（默认编译 srey）
sh mk.sh

# 编译 srey + test（两套都要时用它：单独编 srey 或 test 会删掉另一个）
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

参数顺序：第 1 个为构建目标（空 = 编 srey / `all` / `test` / `clean`），其后为可组合的 flags：`debug`、`asan`、`tsan`、`m32`、`m64`、`arm64`。

---

### CMake（Linux / macOS / Unix）

```sh
# Release：一次产出 bin/srey、bin/test、bin/libsrey.a
cmake -S . -B build
cmake --build build -j

# Debug + ASan/UBSan（依赖要配 python3 tools/deps.py debug）
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug -DSREY_ASAN=ON
cmake --build build-debug -j
```

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `CMAKE_BUILD_TYPE` | `Release` | `Debug` 为 `-O0 -g3`，链 `d_` 后缀的依赖 |
| `SREY_ASAN` | `OFF` | ASan/UBSan（macOS ARM64 须配 Debug） |
| `SREY_TSAN` | `OFF` | ThreadSanitizer，与 `SREY_ASAN` 互斥 |
| `SREY_RDYNAMIC` | `OFF` | Linux 链接加 `-rdynamic`，调用栈带函数名 |

`WITH_SSL` / `WITH_LUA` / `WITH_MIMALLOC` 仍只认 `config.h`，CMake 读它决定链什么；依赖库从 `bin/` 按 `[d]_<arch>` 后缀取，缺了直接报错。
产物与 mk.sh 落在同一处，两边交替用会互相覆盖。

---

### 第三方依赖

openssl 与 mimalloc 由脚本拉取并编译，产物落到 `lib/`（头）与 `bin/`（静态库）。

```sh
python3 tools/deps.py         # 按 config.h 的 WITH_* 决定做哪几个
python3 tools/deps.py clean   # 清构建残留与产物，保留 deps/ 下的源码
python3 tools/deps.py debug   # Debug 依赖（默认 Release）
python3 tools/deps.py m32     # 指定架构；不传按系统探测
```

版本策略：`deps/<name>/` 在就用它，不在才去拉远端最新的正式版。想固定版本就自己把源码
放进 `deps/<name>/`（不必是 git 克隆），想升级就删掉该目录再跑一次。

库名带 `[d]_<arch>` 后缀，各变体在 `bin/` 里共存，换架构或换 debug/release 不必先 `clean`。
Windows 上编 Debug 必须配 `deps.py debug`，否则 mimalloc 的 `/MD` 撞程序的 `/MDd`。
Windows ARM64 的 openssl 一律降到 `/O1`：MSVC 的 `/O2` 会把它编坏，TLS 握手必崩，
与 openssl 版本无关。

---

### Windows

```bat
mk.bat            :: 编译 srey（Release x64）
mk.bat all        :: srey + test
mk.bat clean      :: 清理六组配置
mk.bat debug      :: Debug
mk.bat m32        :: 32 位
```

须在 VS 开发者命令提示符里运行。也可以直接开 `srey.sln`，两者走同一套 `.vcxproj`。

---

## 配置

### 编译期配置（`lib/base/config.h`）

| 宏 | 默认 | 说明 |
|----|------|------|
| `WITH_SSL` | `1` | 启用 OpenSSL |
| `WITH_LUA` | `1` | 启用 Lua |
| `WITH_MIMALLOC` | `0` | 分配器换成 mimalloc（与 ASan / TSan 互斥） |
| `MEMORY_CHECK` | `1` | 退出时检查未释放的内存 |
| `MEMORY_TRACE` | `0` | 记录每次分配的调用栈，退出时打印泄漏位置（需 `MEMORY_CHECK=1`，有性能开销） |
| `KEEPALIVE_TIME` | `30` | TCP keepalive 空闲时间（秒） |
| ... | | |

> 功能开关只改 `config.h`：mk.sh 与 CMake 都从这里读 `WITH_*` 决定链接哪些库，`.vcxproj` 里不设这些宏。
> 事件循环相关的常量（等待超时、单轮慢告警阈值 `EVTASK_SLOW_MS` 等）在 `lib/event/evpub.h`。

### 运行期配置（`bin/configs/config.json`）

| 字段 | 说明 |
|------|------|
| `serviceid` | 服务编号 |
| `nnet` / `nworker` | net 线程数 / worker 线程数，0 = CPU 核数 |
| `loglv` | 日志级别：0 FATAL … 4 DEBUG |
| `stacksize` | 协程栈字节数，0 用默认 |
| `twqueuelens` / `logqueuelens` | 时间轮与日志队列长度 |
| `dns` | DNS 服务器 |
| `script` | Lua 脚本目录 |
| `debug` | 调试控制台：`{ name, ip, port }` |
| `harbor` | 跨服通信：`{ name, ssl, ip, port }`；跨节点身份由 mTLS 保证，`ssl` 填已注册的证书名，该证书须设 `SSL_VERIFY_PEER \| SSL_VERIFY_FAIL_IF_NO_PEER_CERT` |

---

## 核心概念

### Task（任务）

Task 是框架的调度单元：每个 task 有自己的消息队列，消息交给 worker 线程池执行，同一 task 同一时刻只在一个线程上跑。

```c
// 创建并注册 Task（name 为字符串任务名，quecap=0 用默认队列容量）
void my_service(loader_ctx *loader, const char *name) {
    task_ctx *task = task_new(loader, name, 0, NULL, NULL, NULL);
    if (ERR_OK != task_register(task, _startup, _closing)) {
        task_free(task);// 重名注册失败，所有权仍在调用方
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

// 接收回调：sk 是连接标识；slice 非 0 表示分片消息（WebSocket 分片 / HTTP chunked）
static void _net_recv(task_ctx *task, sock_ctx *sk, subtype_t pktype,
    uint8_t client, uint8_t slice, void *data, size_t size) {
    // 处理数据
}
```

**Task 间通信**

```c
// 单向请求（不等待响应）；dst 为目标 task 指针，reqtype 标识请求类型
task_call(dst, reqtype, data, size, copy);

// 请求/响应（协程内挂起等待）；dst 为目标 task，src 为当前 task
// 返回响应数据，仅在下次挂起（再调任意 coro_* API）前有效，需要保留请自行拷贝
int32_t erro = 0;
size_t resp_size = 0;
void *resp = coro_request(dst, task, reqtype, data, size, copy, &erro, &resp_size);
```

**绑到 net 线程**

```c
// 之后这个 task 的全部消息都在第 0 个 net 线程本轮事件派发完之后执行，它发起的连接也落在这条线程上。
// 回调里不能阻塞或做长计算：会卡住该线程上的所有连接
task_bind_net(task, 0);

// 多核监听：每个 net 线程放一个绑在本线程上的组员，由 head 来 listen，
// 新连接落在哪个 net 线程就交给那个线程的组员（handles 个数等于 loader_nnet(loader)）
task_accept_group(head, handles, n);
```

---

### 协程

协程 task 用 `coro_task_register` 创建，它的 startup 与各消息回调都跑在协程里；`coro_*` 挂起接口只能在这类 task 的协程里调用：

```c
task_ctx *task = coro_task_register(loader, "client", 0, _startup, _closing, NULL, NULL);

// 发起连接并等待结果（只挂起协程，不阻塞线程）
// 参数：task, pktype, evssl, ip, port, netev, extra, &sk
sock_ctx sk;
if (ERR_OK == coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", 80, 0, NULL, &sk)) {
    // 发送并等待响应；返回响应数据（下次挂起前有效，需保留请拷贝），NULL 为失败/超时/已断
    size_t resp_size = 0;
    void *resp = coro_send(task, &sk, data, size, &resp_size, 1);
}

// 非阻塞睡眠
coro_sleep(task, 1000); // 1000ms

// SSL 握手升级（client=1 作为客户端）
coro_ssl_exchange(task, &sk, 1, evssl);
```

---

### SSL

```c
// 加载证书创建 SSL 上下文（ca, cert, key, 证书类型），PKCS12 用 evssl_p12_new(p12, pwd)
evssl_ctx *ssl = evssl_new("ca.crt", "server.crt", "server.key", SSL_FILETYPE_PEM);
// 注册名字后，Lua 侧（SSL_NAME）与 harbor 配置按名字取用
evssl_register("server", ssl);

// 监听时启用 SSL
task_listen(task, PACK_HTTP, ssl, "0.0.0.0", 443, &id, 0);

// 连接时启用 SSL
coro_connect(task, PACK_HTTP, ssl, "127.0.0.1", 443, 0, NULL, &sk);
```

测试用证书由 `bin/keys/create.sh`（Windows 用 `create.bat`）生成，不入库。

---

## 协议使用

### HTTP

```c
// 在 _net_recv 里回响应
binary_ctx bwriter;
binary_init_write(&bwriter, 0, 0);
http_pack_resp(&bwriter, 200);
http_pack_content(&bwriter, body, body_len);
ev_send(&task->loader->netev, sk, bwriter.data, bwriter.offset, 0);// copy=0，缓冲区交给事件层释放
```

分块发送（chunked）见 `lib/protocol/http.h` 的 `http_pack_chunked`。

### MySQL / PostgreSQL / SMTP

都在协程 task 里用，接口在 `lib/srey/coro_utils.h`：

```c
// mysql：参数 mysql, ip, port, evssl, user, password, database, charset, maxpk
mysql_ctx mysql;
mysql_init(&mysql, "127.0.0.1", 3306, NULL, "root", "password", "mydb", "utf8mb4", 0);
mysql_connect(task, &mysql);
mysql_query(&mysql, "SELECT 1", NULL, _on_result, udata);// 结果集逐个交给回调

// pgsql
pgsql_ctx pg;
pgsql_init(&pg, "127.0.0.1", 5432, NULL, "postgres", "password", "mydb");
pgsql_connect(task, &pg);
pgpack_ctx *res = pgsql_query(&pg, "SELECT 1");// NULL 为失败

// smtp（ip 须是 IP，域名先用 dns_lookup 解析）
smtp_ctx smtp;
smtp_init(&smtp, "127.0.0.1", 465, ssl, "user@example.com", "password");
smtp_connect(task, &smtp);
smtp_send(&smtp, &mail);
```

### Redis

```c
// redis_connect 建连并完成认证，之后用 coro_send 发 RESP 命令；支持 RESP2 / RESP3
// Lua 层封装见 bin/script/lib/redis.lua
redis_connect(task, NULL, "127.0.0.1", 6379, "password", 0, &sk);
```

### MongoDB

```c
// 使用 PACK_MONGO（Lua 层 PACK_TYPE.MGDB）；C 接口见 coro_utils.h 的 mongo_*，Lua 层封装见 bin/script/lib/mongo.lua
```

---

## Lua 脚本

### HTTP 服务示例

```lua
local srey = require("lib.srey")
local http = require("lib.http")

srey.startup(function()
    srey.on_recved(function(pktype, sk, client, slice, data, size)
        http.response(sk, 200, nil, "hello srey")
    end)
    if ERR_FAILED == srey.listen(PACK_TYPE.HTTP, SSL_NAME.NONE, "0.0.0.0", 8080) then
        WARN("listen error")
    end
end)
```

### WebSocket 服务示例

服务端用 `srey.websock`（C 绑定层）解帧、打包帧，配合 `srey.send` 发送：

```lua
local srey    = require("lib.srey")
local websock = require("srey.websock")

srey.startup(function()
    srey.on_recved(function(pktype, sk, client, slice, data, size)
        local frame = websock.unpack(data)
        if 0x01 == frame.prot then       -- TEXT，回显
            local pack, psize = websock.pack_text(0, 1, frame.data, frame.size)
            srey.send(sk, pack, psize, 0)
        elseif 0x09 == frame.prot then   -- PING → PONG
            local pack, psize = websock.pack_pong(0)
            srey.send(sk, pack, psize, 0)
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
local t = task.register("httpd", "httpd", 0)

-- 可选：绑到第 0 个 net 线程（-1 解绑），注册后再绑时 startup 那一轮仍在 worker 上
task.bind_net(t, 0)
```

---

## Harbor（跨服通信）

Harbor 提供多服务器节点间的透明通信，跑在 HTTP 之上。C 层用 `harbor_start` 启动节点，通常由 `config.json` 的 `harbor` 块配置、启动时自动拉起：

```c
// 证书要求对端必须出示证书（mTLS），两位都得有：只设 PEER 时对端不交证书照样握手成功
evssl_ctx *hbssl = evssl_new("ca.crt", "node.crt", "node.key", SSL_FILETYPE_PEM);
evssl_verify(hbssl, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
evssl_register("harbor", hbssl);
// 启动 harbor 节点（task 名、证书名、监听地址、端口）；NULL 或 "" 为明文无鉴权（仅限受信内网，启动时打 WARN）
harbor_start(loader, "harbor", "harbor", "0.0.0.0", 8080);
```

Lua 层经由连到对端 harbor 的连接 `sk` 向远端 task 发起调用 / 请求（`dst` 为远端 task 的数字句柄）：

```lua
local ok = srey.net_call(sk, dst, reqtype, data, size)
local ok, rdata, rsize, erro = srey.net_request(sk, dst, reqtype, data, size)
```

---

## 运行测试

数据库与 MQTT 相关用例要先起本机后端服务（端口、账号与测试代码一致，详见 `测试环境配置.txt`）：

```sh
docker compose up -d
```

C 测试（CuTest 单元测试 + loader 拉起的集成测试）：

```sh
sh mk.sh test
./bin/test      # 全部出结果后自动汇总并退出；有用例卡住 300 秒后列出未完成项再收尾
```

Lua 测试（`bin/script/test` 下全部模块，含用 python3 跑 `bin/py_assist` 的端到端用例）：

```sh
sh mk.sh
./bin/srey -d   # -d 为前台模式，必带；跑完打印汇总后不退出，Ctrl+C 结束
```

两套都要跑时用 `sh mk.sh all` 一次编出来。只跑某个 C 模块：在 `test/main.c` 注释掉不需要的 `test_*()` 调用后重编。

更多用法参考 `test/` 与 `bin/script/test/` 下的用例。
