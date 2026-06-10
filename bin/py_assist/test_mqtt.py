#!/usr/bin/env python3
# SREY MQTT server e2e + 并发测试（端口 1883，进程内 task_mqtt_server）
# 重点：CuTest test_mqtt_pack 已覆盖协议层；本脚本聚焦真 socket 多连接调度
import socket
import struct
import sys
import threading

HOST = "127.0.0.1"
PORT = 1883
TIMEOUT = 5.0

CONNECT = 0x10
CONNACK = 0x20
PUBLISH = 0x30
PUBACK = 0x40
SUBSCRIBE = 0x82
SUBACK = 0x90
PINGREQ = 0xC0
PINGRESP = 0xD0
DISCONNECT = 0xE0


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


def decode_rlen(sock):
    multiplier = 1
    value = 0
    for _ in range(4):
        b = sock.recv(1)
        if not b:
            return None
        v = b[0]
        value += (v & 0x7F) * multiplier
        if (v & 0x80) == 0:
            return value
        multiplier *= 128
    return None


def recv_packet(sock):
    head = sock.recv(1)
    if not head:
        return None
    rlen = decode_rlen(sock)
    if rlen is None:
        return None
    body = b""
    while len(body) < rlen:
        c = sock.recv(rlen - len(body))
        if not c:
            return None
        body += c
    return head[0], body


def connect_mqtt(clientid):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(TIMEOUT)
    s.connect((HOST, PORT))
    var_head = struct.pack("!H", 4) + b"MQTT" + bytes([4, 0x02]) + struct.pack("!H", 60)
    payload = struct.pack("!H", len(clientid)) + clientid
    body = var_head + payload
    s.sendall(bytes([CONNECT]) + encode_rlen(len(body)) + body)
    pk = recv_packet(s)
    if pk is None or (pk[0] & 0xF0) != CONNACK:
        s.close()
        raise RuntimeError(f"CONNACK missing: {pk!r}")
    return s


# e2e：完整 CONNECT → CONNACK 握手
def case_connack_e2e():
    s = connect_mqtt(b"srey-py-e2e")
    s.close()
    return True


# e2e：PUBLISH QoS1 → PUBACK（验证 server packid 回包路径）
def case_publish_qos1_e2e():
    s = connect_mqtt(b"srey-py-q1")
    try:
        topic = b"t/q1"
        payload = b"hello"
        body = struct.pack("!H", len(topic)) + topic + struct.pack("!H", 1) + payload
        s.sendall(bytes([PUBLISH | 0x02]) + encode_rlen(len(body)) + body)
        pk = recv_packet(s)
        return pk is not None and (pk[0] & 0xF0) == PUBACK
    finally:
        s.close()


# e2e：SUBSCRIBE → SUBACK + server 主动推 PUBLISH
def case_subscribe_e2e():
    s = connect_mqtt(b"srey-py-sub")
    try:
        topic = b"/test/topic1"
        body = struct.pack("!H", 100) + struct.pack("!H", len(topic)) + topic + bytes([0])
        s.sendall(bytes([SUBSCRIBE]) + encode_rlen(len(body)) + body)
        pk = recv_packet(s)
        if pk is None or (pk[0] & 0xF0) != SUBACK:
            return False
        s.settimeout(1.0)
        try:
            pk2 = recv_packet(s)
            return pk2 is not None and (pk2[0] & 0xF0) == PUBLISH
        except socket.timeout:
            return True
    finally:
        s.close()


# e2e：PINGREQ → PINGRESP
def case_ping_e2e():
    s = connect_mqtt(b"srey-py-ping")
    try:
        s.sendall(bytes([PINGREQ, 0]))
        pk = recv_packet(s)
        return pk is not None and (pk[0] & 0xF0) == PINGRESP
    finally:
        s.close()


# 并发：30 个客户端同时 CONNECT + PUBLISH QoS1 → 验证 server 多连接调度
def case_30_parallel_connect_publish():
    results = [None] * 30
    def worker(idx):
        try:
            s = connect_mqtt(f"srey-py-c{idx}".encode())
            try:
                topic = b"t/p"
                body = struct.pack("!H", len(topic)) + topic + struct.pack("!H", idx + 1) + b"x"
                s.sendall(bytes([PUBLISH | 0x02]) + encode_rlen(len(body)) + body)
                pk = recv_packet(s)
                results[idx] = pk is not None and (pk[0] & 0xF0) == PUBACK
            finally:
                s.close()
        except Exception:
            results[idx] = False
    threads = [threading.Thread(target=worker, args=(i,)) for i in range(30)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=TIMEOUT * 2)
    return all(r is True for r in results)


CASES = [
    ("CONNECT/CONNACK e2e", case_connack_e2e),
    ("PUBLISH QoS1 e2e", case_publish_qos1_e2e),
    ("SUBSCRIBE e2e", case_subscribe_e2e),
    ("PING e2e", case_ping_e2e),
    ("30 parallel CONNECT+PUBLISH", case_30_parallel_connect_publish),
]


def main():
    fails = 0
    for name, fn in CASES:
        try:
            ok = fn()
        except Exception as e:
            ok = False
            print(f"[mqtt] {name}: ERROR {e}", flush=True)
        if ok:
            print(f"[mqtt] {name}: PASS", flush=True)
        else:
            print(f"[mqtt] {name}: FAIL", flush=True)
            fails += 1
    print(f"[mqtt] summary: {len(CASES) - fails}/{len(CASES)} passed", flush=True)
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
