#!/usr/bin/env python3
# SREY 多协议混合并发：同时打 HTTP + WS + MQTT 各 N 个连接，压 lib/event 多 listener 协同
# CuTest / 各 task_*_client / 其他 Python 脚本不覆盖此场景
import os
import socket
import struct
import sys
import threading

HOST = "127.0.0.1"
HTTP_PORT = 15002
WS_PORT = 15003
MQTT_PORT = 1883
TIMEOUT = 5.0


# ---- HTTP worker ----
def http_worker_once():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(TIMEOUT)
    try:
        s.connect((HOST, HTTP_PORT))
        s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
        buf = b""
        while b"\r\n\r\n" not in buf:
            c = s.recv(4096)
            if not c:
                return False
            buf += c
        head, body = buf.split(b"\r\n\r\n", 1)
        cl = 0
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                cl = int(line.split(b":", 1)[1].strip())
        while len(body) < cl:
            c = s.recv(cl - len(body))
            if not c:
                break
            body += c
        return b" 200 " in head and b"ok" in body
    finally:
        s.close()


# ---- WS worker ----
WS_KEY = b"dGhlIHNhbXBsZSBub25jZQ=="


def ws_worker_once():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(TIMEOUT)
    try:
        s.connect((HOST, WS_PORT))
        req = (
            b"GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            b"Sec-WebSocket-Key: " + WS_KEY + b"\r\nSec-WebSocket-Version: 13\r\n\r\n"
        )
        s.sendall(req)
        buf = b""
        while b"\r\n\r\n" not in buf:
            c = s.recv(4096)
            if not c:
                return False
            buf += c
        if b" 101 " not in buf.split(b"\r\n", 1)[0]:
            return False
        # 发一个 mask 过的 text 帧 "hi" → 期望 echo
        mask = os.urandom(4)
        payload = bytes(p ^ mask[i % 4] for i, p in enumerate(b"hi"))
        frame = struct.pack("!BB", 0x81, 0x82) + mask + payload
        s.sendall(frame)
        head = s.recv(2)
        if len(head) < 2:
            return False
        plen = head[1] & 0x7F
        masked = (head[1] >> 7) & 1
        body = b""
        while len(body) < plen:
            c = s.recv(plen - len(body))
            if not c:
                break
            body += c
        if masked:
            return False  # server 给客户端的帧不应 mask
        return (head[0] & 0x0F) == 0x1 and body == b"hi"
    finally:
        s.close()


# ---- MQTT worker ----
def encode_rlen(n):
    out = b""
    while True:
        byte = n & 0x7F
        n >>= 7
        if n > 0:
            byte |= 0x80
        out += bytes([byte])
        if n == 0:
            return out


def mqtt_worker_once(idx):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(TIMEOUT)
    try:
        s.connect((HOST, MQTT_PORT))
        clientid = f"mix-{idx}".encode()
        var_head = struct.pack("!H", 4) + b"MQTT" + bytes([4, 0x02]) + struct.pack("!H", 60)
        payload = struct.pack("!H", len(clientid)) + clientid
        body = var_head + payload
        s.sendall(bytes([0x10]) + encode_rlen(len(body)) + body)
        # 期望 CONNACK：固定头 0x20 + rlen + 2 字节
        head = s.recv(1)
        if not head or (head[0] & 0xF0) != 0x20:
            return False
        rlen_b = s.recv(1)
        if not rlen_b:
            return False
        n = rlen_b[0] & 0x7F
        # 简化：rlen < 128 单字节就够
        body = b""
        while len(body) < n:
            c = s.recv(n - len(body))
            if not c:
                break
            body += c
        return True
    finally:
        s.close()


# 60 个线程同时开（HTTP 20 / WS 20 / MQTT 20）
N_EACH = 20


def case_mixed_60_parallel():
    results = [None] * (N_EACH * 3)

    def http_w(i):
        try:
            results[i] = http_worker_once()
        except Exception:
            results[i] = False

    def ws_w(i):
        try:
            results[i] = ws_worker_once()
        except Exception:
            results[i] = False

    def mqtt_w(i):
        try:
            results[i] = mqtt_worker_once(i)
        except Exception:
            results[i] = False

    threads = []
    for i in range(N_EACH):
        threads.append(threading.Thread(target=http_w, args=(i,)))
        threads.append(threading.Thread(target=ws_w, args=(N_EACH + i,)))
        threads.append(threading.Thread(target=mqtt_w, args=(2 * N_EACH + i,)))
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=TIMEOUT * 2)
    ok_cnt = sum(1 for r in results if r is True)
    print(f"[mixed] 60-parallel breakdown: {ok_cnt}/{len(results)} ok", flush=True)
    return ok_cnt == len(results)


CASES = [
    ("60-parallel HTTP+WS+MQTT", case_mixed_60_parallel),
]


def main():
    fails = 0
    for name, fn in CASES:
        try:
            ok = fn()
        except Exception as e:
            ok = False
            print(f"[mixed] {name}: ERROR {e}", flush=True)
        if ok:
            print(f"[mixed] {name}: PASS", flush=True)
        else:
            print(f"[mixed] {name}: FAIL", flush=True)
            fails += 1
    print(f"[mixed] summary: {len(CASES) - fails}/{len(CASES)} passed", flush=True)
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
