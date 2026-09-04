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
# RFC 6455 §1.3 的样例 key 与它对应的固定 accept 值。srey 的 C 客户端与 Lua 客户端
# 都拿服务端同一个 _websock_sign 算期望值、整条链自洽，这里是全仓唯一的独立参照：
# SIGNKEY 打错一字或摘要算法换掉，只有这条断言会红
WS_ACCEPT = b"s3pPLMBiTxaQ9kYGzzhZRbK+xOo="


def connect_ws():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(TIMEOUT)
    try:
        return _ws_handshake(s)
    except Exception:
        s.close()   # 握手任一步抛出都得把 socket 关掉，不然泄到进程退出
        raise


def _ws_handshake(s):
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
    if WS_ACCEPT not in buf:
        raise RuntimeError(f"Sec-WebSocket-Accept wrong or missing: {buf[:200]!r}")
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
        # 定长字段一律收满再解：TCP 可以在任意位置切开，短读一次就把长度解错，
        # 后面按错长度去读 payload，结果是"偶发的假红"
        def _recvn(n):
            buf = b""
            while len(buf) < n:
                c = sock.recv(n - len(buf))
                if not c:
                    return None
                buf += c
            return buf
        head = _recvn(2)
        if head is None:
            return None
        b0, b1 = head[0], head[1]
        opcode = b0 & 0x0F
        masked = (b1 >> 7) & 1
        plen = b1 & 0x7F
        if plen == 126:
            ext = _recvn(2)
            if ext is None:
                return None
            plen = struct.unpack("!H", ext)[0]
        elif plen == 127:
            ext = _recvn(8)
            if ext is None:
                return None
            plen = struct.unpack("!Q", ext)[0]
        mask_key = b""
        if masked:
            mask_key = _recvn(4)
            if mask_key is None:
                return None
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


# e2e：60KB binary 一帧回显（验证 server 大缓冲 ev_send，WS_MAX_PAYLOAD_LENS 64KB 内）
def case_large_binary_e2e():
    s = connect_ws()
    try:
        payload = b"X" * 60000
        s.sendall(make_frame(0x2, payload))
        r = recv_frame(s)
        # 内容也比：只比长度的话，服务端把大缓冲搬错一段、掩码解错都测不出来
        return r is not None and r[0] == 0x2 and r[1] == payload
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


# e2e：客户端发分片消息（TEXT fin=0 + CONTINUE fin=0 + CONTINUE fin=1），
# 服务端 server_ws.lua 的 slice 分支收齐后回三帧分片 "a"/"b"/"c"。
# 这条分支此前没有任何驱动方，整段删掉也没人发现
def case_fragmented_echo_e2e():
    s = connect_ws()
    try:
        s.sendall(make_frame(0x1, b"frag-", fin=0))    # TEXT，未完
        s.sendall(make_frame(0x0, b"mid-", fin=0))     # CONTINUE，未完
        s.sendall(make_frame(0x0, b"end", fin=1))      # CONTINUE，收尾
        # 服务端回三帧：opcode 依次 TEXT / CONTINUE / CONTINUE，载荷 a / b / c
        got = []
        for _ in range(3):
            r = recv_frame(s)
            if r is None:
                return False
            got.append(r)
        return ([g[0] for g in got] == [0x1, 0x0, 0x0]
                and b"".join(g[1] for g in got) == b"abc")
    finally:
        s.close()


CASES = [
    ("text echo e2e", case_text_echo_e2e),
    ("fragmented echo e2e", case_fragmented_echo_e2e),
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
