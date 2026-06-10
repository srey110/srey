#!/usr/bin/env python3
# SREY HTTP server e2e + lib/event 并发/listener churn 测试（端口 15002）
# CuTest 已覆盖协议层 unpack；本脚本聚焦真实 socket、并发连接、listener 生命周期
import socket
import sys
import threading

HOST = "127.0.0.1"
PORT = 15002
TIMEOUT = 5.0


def connect():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(TIMEOUT)
    s.connect((HOST, PORT))
    return s


# 按 Content-Length / Transfer-Encoding 精确读响应，不依赖 socket timeout
def recv_response(sock):
    sock.settimeout(TIMEOUT)
    buf = b""
    # 先读到 headers 结束
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            return buf
        buf += chunk
        if len(buf) > 1024 * 1024:
            return buf
    head, body = buf.split(b"\r\n\r\n", 1)
    cl = -1
    chunked = False
    for line in head.split(b"\r\n"):
        low = line.lower()
        if low.startswith(b"content-length:"):
            try:
                cl = int(line.split(b":", 1)[1].strip())
            except ValueError:
                pass
        elif low.startswith(b"transfer-encoding:") and b"chunked" in low:
            chunked = True
    if chunked:
        # 读到 "0\r\n\r\n" 终止
        while not body.endswith(b"0\r\n\r\n"):
            chunk = sock.recv(4096)
            if not chunk:
                break
            body += chunk
            if len(body) > 1024 * 1024:
                break
    elif cl >= 0:
        while len(body) < cl:
            chunk = sock.recv(min(4096, cl - len(body)))
            if not chunk:
                break
            body += chunk
    return head + b"\r\n\r\n" + body


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
        return b" 200 " in resp and resp.count(b"A") >= 50000
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
        return b" 200 " in resp
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
