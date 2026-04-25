# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**Srey** is a lightweight, high-performance C/Lua async service framework (轻量级 C / Lua 高性能异步服务框架). It uses a task-based coroutine model over a platform-native event loop (IOCP/epoll/kqueue) to enable sync-style async network programming.

Languages: C (C99 with GNU extensions) and Lua 5.4.

## Build Commands

### Using mk.sh (Linux/macOS)

```bash
sh mk.sh            # Release build
DEBUG=1 sh mk.sh    # Debug build (ASan + UBSan)
sh mk.sh test       # Compile test suite
sh mk.sh clean      # Clean build artifacts
```

### Using CMake

```bash
# Configure (macOS with SSL)
cmake -S . -B build -DWITH_LUA=ON -DWITH_SSL=ON \
    -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3

# Configure (Debug)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DWITH_LUA=ON -DWITH_SSL=ON

# Build
cmake --build build
```

CMake options: `WITH_SSL` (default OFF), `WITH_LUA` (default ON).

### Running Tests

```bash
sh mk.sh test   # compile
./bin/test      # run all tests
```

To run a single module's tests, edit `test/main.c` to comment out the unwanted `test_*()` calls, rebuild with `sh mk.sh test`, then run `./bin/test`.

## Architecture

### Execution Model

```
main.c → loader (thread pool init) → tasks → coroutines → event loop
```

1. **Loader** (`lib/srey/loader.c`) starts N network threads and M worker threads.
2. **Tasks** (`lib/srey/task.c`) are logical services — each task owns a message queue and registers startup/closing callbacks.
3. **Coroutines** (`lib/srey/coro.c`, powered by embedded `minicoro.h`) let tasks call blocking I/O (connect, send, sleep) without blocking a thread. The `coro_utils.c` layer provides higher-level helpers (DNS lookup, DB connections).
4. **Event Loop** (`lib/event/event.c`) multiplexes I/O via the platform's best API (IOCP on Windows, epoll on Linux, kqueue on macOS/BSD).
5. **Protocol Layer** (`lib/protocol/`) parses/formats wire data: HTTP, WebSocket, MQTT 3.1.1/5.0, DNS, Redis RESP, MySQL, PostgreSQL, MongoDB, SMTP, and custom protocols.
6. **Harbor** (`lib/services/`) is an optional cross-server mesh for multi-node deployments.

### Core Programming Patterns

**Task / service registration:**
```c
void my_service(loader_ctx *loader, name_t name) {
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, _closing);
}
static void _startup(task_ctx *task) {
    on_recved(task, _net_recv);
    uint64_t id;
    task_listen(task, PACK_HTTP, NULL, "0.0.0.0", 8080, &id, 0);
}
```

**Coroutine-based async I/O (sync style):**
```c
SOCKET fd; uint64_t skid;
coro_connect(task, PACK_HTTP, NULL, "127.0.0.1", 80, &fd, &skid);
void *resp = NULL; size_t resp_size = 0;
coro_send(task, fd, skid, data, size, &resp, &resp_size);
coro_sleep(task, 1000); // ms, non-blocking
```

**Lua service example:**
```lua
local srey = require("lib.srey")
srey.startup(function()
    srey.on_recved(function(pktype, fd, skid, client, slice, data, size)
        -- handle request
    end)
    srey.listen(PACK_TYPE.HTTP, 0, "0.0.0.0", 8080)
end)
```

### Key Directories

| Path | Contents |
|---|---|
| `lib/base/` | OS abstractions, memory macros, type definitions, compile-time config (`config.h`) |
| `lib/containers/` | Arrays, queues, heaps, hash tables, lock-free MPSC queues |
| `lib/crypt/` | AES, DES, MD5, SHA-1/256, HMAC, Base64, CRC, URL encoding, XOR |
| `lib/event/` | Platform event loop + `evssl.c` (OpenSSL TLS wrapper) |
| `lib/protocol/` | HTTP, WebSocket, Redis, DNS, MySQL, PostgreSQL, MQTT, MongoDB, SMTP, custom |
| `lib/srey/` | Core: `task.c`, `coro.c`, `coro_utils.c`, `loader.c`, `minicoro.h` |
| `lib/services/` | Harbor cross-server communication |
| `lib/thread/` | Mutexes, RW locks, spinlocks, condition variables |
| `lib/utils/` | Logging, timers, time wheel, buffers, binary pack/unpack, snowflake IDs, consistent hashing, network address utils |
| `lualib/` | Embedded Lua 5.4, C-to-Lua bindings, cJSON, LFS, protobuf |
| `srey/` | Main executable entry (`main.c`), cJSON library |
| `test/` | CuTest unit + integration tests |
| `bin/configs/` | Runtime config (`config.json`) |
| `bin/script/` | Lua startup scripts |

### Compile-time Configuration (`lib/base/config.h`)

Key knobs: `MEMORY_CHECK`, `WITH_SSL`, `WITH_LUA`, `KEEPALIVE_TIME`, `MAX_PACK_SIZE`, `MAX_RECV_SIZE`, `MAX_SEND_SIZE`, `EVENT_WAIT_TIMEOUT`.

### Runtime Configuration (`bin/configs/config.json`)

Key fields: `nnet` / `nworker` (thread counts, 0 = CPU cores), `loglv` (0 ERROR … 5 TRACE), `stacksize` (coroutine stack bytes), `dns`, `script` (Lua startup file), `harborname/port/ip/ssl/key`.

## Test Framework

Tests use **CuTest** (bundled in `test/`). The `test/main.c` aggregates suites:
- `test_base`, `test_containers`, `test_crypt`, `test_utils`, `test_thread`, `test_protocol` — pure in-memory unit tests
- `test_srey` — integration tests that spin up the full loader + network event loop

## Code Style

所有代码注释使用中文书写。

对外函数（头文件中声明的公开接口）使用 XML 风格注释，参照 `lib/protocol/websock.h` 的格式：

```c
/// <summary>
/// 函数功能描述
/// </summary>
/// <param name="参数名">参数说明</param>
/// <returns>返回值说明</returns>
void *my_function(int32_t mask, size_t *size);
```

内部函数（以 `_` 开头或未在头文件中声明）使用单行 `//` 注释即可。

## Compiler Warnings

Warnings are treated as hard errors via `-Wall -Wextra -Wshadow -Wstrict-prototypes -Werror=implicit-function-declaration`. Fix all warnings before committing.
