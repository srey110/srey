#!/usr/bin/env python3
# SREY HTTP server e2e + lib/event 并发/listener churn 测试（端口 15002）
# CuTest 已覆盖协议层 unpack；本脚本聚焦真实 socket、并发连接、listener 生命周期
import select
import socket
import sys
import threading

HOST = "127.0.0.1"
PORT = 15002
TIMEOUT = 5.0


def connect():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(TIMEOUT)
    try:
        s.connect((HOST, PORT))
    except OSError:
        s.close()   # connect 抛出时 socket 已建好，不关就泄到进程退出
        raise
    return s


# 按 Content-Length / Transfer-Encoding 精确读响应，不依赖 socket timeout
def recv_response(sock):
    sock.settimeout(TIMEOUT)
    buf = b""
    # 先读到 headers 结束
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError(f"EOF in headers: {buf[:200]!r}")
        buf += chunk
        if len(buf) > 1024 * 1024:
            raise RuntimeError("header block over 1MB")
    head, body = buf.split(b"\r\n\r\n", 1)
    cl = -1
    chunked = False
    for line in head.split(b"\r\n"):
        low = line.lower()
        if low.startswith(b"content-length:"):
            # 解析不出来就是服务端发了个坏 Content-Length，不能吞成"没这个头"：
            # 吞掉的话 cl 保持 -1，下面既不按长度读也不按 chunked 读，body 直接算收完了
            cl = int(line.split(b":", 1)[1].strip())
        elif low.startswith(b"transfer-encoding:") and b"chunked" in low:
            chunked = True
    if chunked:
        # 读到 "0\r\n\r\n" 终止
        while not body.endswith(b"0\r\n\r\n"):
            chunk = sock.recv(4096)
            if not chunk:
                raise RuntimeError(f"EOF in chunked body after {len(body)} bytes")
            body += chunk
            if len(body) > 1024 * 1024:
                raise RuntimeError("chunked body over 1MB")
    elif cl < 0:
        # 既无 Content-Length 又非 chunked：本测试里的服务端一律带其一，
        # 静默放行会把"帧头丢了"报成通过
        raise RuntimeError("response has neither Content-Length nor chunked framing")
    else:
        while len(body) < cl:
            chunk = sock.recv(min(4096, cl - len(body)))
            if not chunk:
                raise RuntimeError(f"EOF in body: got {len(body)}/{cl}")
            body += chunk
    return head + b"\r\n\r\n" + body


# 解 chunked body。长度前缀写错（十进制/错值）、块尾漏 CRLF、终止块不发，都会在这里抛错；
# 只查状态码的话这三种回归全看不见
def dechunk(body):
    out = b""
    while True:
        nl = body.find(b"\r\n")
        if nl < 0:
            raise RuntimeError(f"chunk size line without CRLF: {body[:40]!r}")
        n = int(body[:nl].split(b";")[0], 16)
        body = body[nl + 2:]
        if 0 == n:
            return out
        if len(body) < n + 2:
            raise RuntimeError(f"chunk shorter than declared {n}")
        if b"\r\n" != body[n:n + 2]:
            raise RuntimeError(f"chunk {n} not terminated by CRLF")
        out += body[:n]
        body = body[n + 2:]


def do_get():
    s = connect()
    try:
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        resp = recv_response(s)
        return b" 200 " in resp and b"ok" in resp
    finally:
        s.close()


# e2e：GET 一回合
def case_get_e2e():
    return do_get()


# e2e：50KB POST body 真 socket 大 payload 回显（CuTest 内存测试无法覆盖）
def case_post_50kb_echo():
    s = connect()
    try:
        body = b"A" * 50000
        req = (
            b"POST / HTTP/1.1\r\n"
            b"Host: x\r\n"
            b"Content-Length: " + str(len(body)).encode() + b"\r\n"
            b"\r\n" + body
        )
        s.sendall(req)
        resp = recv_response(s)
        if b" 200 " not in resp:
            return False
        # 逐字节比对而非 count('A')>=50000：那种写法抓不到"回显两遍并把 CL 一起改大"，
        # 而分块发送的 offset 记账错误正是这个形态
        return body == resp.split(b"\r\n\r\n", 1)[1]
    finally:
        s.close()


# e2e：chunked request → server 三帧 chunked response（PROT_SLICE_END server 路径）
def case_chunked_e2e():
    s = connect()
    try:
        head = (
            b"POST / HTTP/1.1\r\n"
            b"Host: x\r\n"
            b"Transfer-Encoding: chunked\r\n"
            b"\r\n"
        )
        s.sendall(head + b"5\r\nhello\r\n0\r\n\r\n")
        resp = recv_response(s)
        if b" 200 " not in resp:
            return False
        rhead, rbody = resp.split(b"\r\n\r\n", 1)
        if b"chunked" not in rhead.lower():
            raise RuntimeError(f"response not chunked: {rhead[:200]!r}")
        # 两侧服务端都回三帧 "a"/"b"/终止块（test/task_http_server.c 与 server_http.lua），
        # 故解出来必须正好是 "ab"：中间帧被吞、块长写错、终止块缺失都会在此变红
        return b"ab" == dechunk(rbody)
    finally:
        s.close()


# 并发：50 个 TCP 同时打开 + GET → 压 lib/event 多连接 accept + send/recv 调度
def case_50_parallel_get():
    results = [None] * 50
    def worker(idx):
        try:
            results[idx] = do_get()
        except Exception:
            results[idx] = False
    threads = [threading.Thread(target=worker, args=(i,)) for i in range(50)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=TIMEOUT * 2)
    return all(r is True for r in results)


# Listener churn：100 次顺序 connect/close → 压 lib/event accept + close 流水
# 覆盖最近 KQUEUE 同 udata kevent 合并 + _lsn_ungrab 重构修复路径
def case_100_seq_churn():
    for i in range(100):
        if not do_get():
            print(f"[http] churn iter {i} did not get 200 ok", flush=True)
            return False
    return True


# 半开连接：5 个 TCP 连后不发数据 → 验证 server 能持有空闲连接 + 后续连接不受影响
def case_half_open_5():
    import time
    holds = []
    try:
        for _ in range(5):
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(TIMEOUT)
            s.connect((HOST, PORT))
            holds.append(s)
        time.sleep(2.0)
        # 这 5 条空闲连接必须还活着。以前这里只做一次新连接 GET，服务端 accept 后就把
        # 不发数据的连接踢掉（或 KEEPALIVE_TIME 调到 1s）照样通过，用例退化成"多花 2 秒的 GET"
        for i, h in enumerate(holds):
            rd, _, _ = select.select([h], [], [], 0)
            if rd and b"" == h.recv(1):
                raise RuntimeError(f"idle conn {i} closed by server")
        # sanity：新连接 GET 仍能正常工作
        if not do_get():
            return False
        return True
    finally:
        for s in holds:
            try:
                s.close()
            except Exception:
                pass


# 慢速逐字节发送 GET 请求 → 测 SREY HTTP parser 处理 partial recv
def case_slow_byte_send():
    import time
    s = connect()
    try:
        req = b"GET / HTTP/1.1\r\nHost: x\r\n\r\n"
        for b in req:
            s.sendall(bytes([b]))
            time.sleep(0.02)  # 20ms 一字节，约 0.7s 完成发送
        resp = recv_response(s)
        return b" 200 " in resp and b"ok" in resp
    finally:
        s.close()


CASES = [
    ("GET e2e", case_get_e2e),
    ("POST 50KB body e2e", case_post_50kb_echo),
    ("chunked e2e", case_chunked_e2e),
    ("50 parallel GET", case_50_parallel_get),
    ("100 seq connect/close churn", case_100_seq_churn),
    ("5 half-open + sanity", case_half_open_5),
    ("slow byte-by-byte GET", case_slow_byte_send),
]


def main():
    fails = 0
    for name, fn in CASES:
        try:
            ok = fn()
        except Exception as e:
            ok = False
            print(f"[http] {name}: ERROR {e}", flush=True)
        if ok:
            print(f"[http] {name}: PASS", flush=True)
        else:
            print(f"[http] {name}: FAIL", flush=True)
            fails += 1
    print(f"[http] summary: {len(CASES) - fails}/{len(CASES)} passed", flush=True)
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
