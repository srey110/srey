#!/usr/bin/env python3
# SREY WebSocket server e2e + 并发测试（端口 15003）
# 重点：CuTest test_websock_* 已覆盖协议帧 unpack；本脚本聚焦真 socket、handshake、并发
import os
import socket
import struct
import sys
import threading

HOST = "127.0.0.1"
PORT = 15003
TIMEOUT = 5.0
WS_KEY = b"dGhlIHNhbXBsZSBub25jZQ=="


def connect_ws():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(TIMEOUT)
    s.connect((HOST, PORT))
    req = (
        b"GET /ws HTTP/1.1\r\n"
        b"Host: x\r\n"
        b"Upgrade: websocket\r\n"
        b"Connection: Upgrade\r\n"
        b"Sec-WebSocket-Key: " + WS_KEY + b"\r\n"
        b"Sec-WebSocket-Version: 13\r\n"
        b"\r\n"
    )
    s.sendall(req)
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            raise RuntimeError("handshake closed prematurely")
        buf += chunk
    if b" 101 " not in buf.split(b"\r\n", 1)[0]:
        raise RuntimeError(f"handshake status not 101: {buf[:80]!r}")
    return s


def make_frame(opcode, payload, fin=1, mask_key=None):
    if mask_key is None:
        mask_key = os.urandom(4)
    b0 = ((fin & 1) << 7) | (opcode & 0x0F)
    n = len(payload)
    if n < 126:
        header = struct.pack("!BB", b0, 0x80 | n)
    elif n < 65536:
        header = struct.pack("!BBH", b0, 0x80 | 126, n)
    else:
        header = struct.pack("!BBQ", b0, 0x80 | 127, n)
    masked = bytes(p ^ mask_key[i % 4] for i, p in enumerate(payload))
    return header + mask_key + masked


def recv_frame(sock):
    try:
        head = b""
        while len(head) < 2:
            c = sock.recv(2 - len(head))
            if not c:
                return None
            head += c
        b0, b1 = head[0], head[1]
        opcode = b0 & 0x0F
        masked = (b1 >> 7) & 1
        plen = b1 & 0x7F
        if plen == 126:
            plen = struct.unpack("!H", sock.recv(2))[0]
        elif plen == 127:
            plen = struct.unpack("!Q", sock.recv(8))[0]
        mask_key = sock.recv(4) if masked else b""
        payload = b""
        while len(payload) < plen:
            c = sock.recv(plen - len(payload))
            if not c:
                return None
            payload += c
        if masked:
            payload = bytes(p ^ mask_key[i % 4] for i, p in enumerate(payload))
        return opcode, payload
    except (socket.timeout, ConnectionError, OSError):
        return None


def do_handshake_echo():
    s = connect_ws()
    try:
        s.sendall(make_frame(0x1, b"hi"))
        r = recv_frame(s)
        return r is not None and r[0] == 0x1 and r[1] == b"hi"
    finally:
        s.close()


# e2e：完整握手 + text 回显（CuTest 不经过真 server）
def case_text_echo_e2e():
    return do_handshake_echo()


# e2e：binary 回显（256 字节，常规小帧）
def case_binary_echo_e2e():
    s = connect_ws()
    try:
        payload = bytes(range(256))
        s.sendall(make_frame(0x2, payload))
        r = recv_frame(s)
        return r is not None and r[0] == 0x2 and r[1] == payload
    finally:
        s.close()


# e2e：PING → PONG（覆盖 server 端控制帧响应路径）
def case_ping_pong_e2e():
    s = connect_ws()
    try:
        s.sendall(make_frame(0x9, b"pp"))
        r = recv_frame(s)
        return r is not None and r[0] == 0xA
    finally:
        s.close()


# e2e：60KB binary 一帧回显（验证 server 大缓冲 ev_send，MAX_PACK_SIZE 64KB 内）
def case_large_binary_e2e():
    s = connect_ws()
    try:
        payload = b"X" * 60000
        s.sendall(make_frame(0x2, payload))
        r = recv_frame(s)
        return r is not None and r[0] == 0x2 and len(r[1]) == 60000
    finally:
        s.close()


# 并发：50 个 WS 同时握手 + 回显 → 压 SREY 的 HTTP upgrade + WS 多连接调度
def case_50_parallel_echo():
    results = [None] * 50
    def worker(idx):
        try:
            results[idx] = do_handshake_echo()
        except Exception:
            results[idx] = False
    threads = [threading.Thread(target=worker, args=(i,)) for i in range(50)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=TIMEOUT * 2)
    return all(r is True for r in results)


CASES = [
    ("text echo e2e", case_text_echo_e2e),
    ("binary echo e2e", case_binary_echo_e2e),
    ("PING -> PONG e2e", case_ping_pong_e2e),
    ("60KB binary e2e", case_large_binary_e2e),
    ("50 parallel handshake+echo", case_50_parallel_echo),
]


def main():
    fails = 0
    for name, fn in CASES:
        try:
            ok = fn()
        except Exception as e:
            ok = False
            print(f"[ws] {name}: ERROR {e}", flush=True)
        if ok:
            print(f"[ws] {name}: PASS", flush=True)
        else:
            print(f"[ws] {name}: FAIL", flush=True)
            fails += 1
    print(f"[ws] summary: {len(CASES) - fails}/{len(CASES)} passed", flush=True)
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
