#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# SREY SSL 数据期 TLS 1.3 KeyUpdate e2e 测试（默认端口 15443）
#
# 验证 lib/event/usock.c 在「数据传输期」处理 TLS 1.3 KeyUpdate 的正确性（方向 A）：
#   SSL_read 收到对端 KeyUpdate 后需写响应记录（WANT_WRITE），srey 由事件循环驱动写就绪重试；
#   KeyUpdate 后同一连接仍能正常收发 HTTP —— srey 不停滞、不忙转、不断连。
# 背景：srey evssl 已设 SSL_OP_NO_RENEGOTIATION 显式禁用 TLS 1.2 重协商（OpenSSL 3.0 默认也拒
#   client 发起的重协商），故不测重协商；TLS 1.3 KeyUpdate 是独立 post-handshake 机制，不受影响。
#
# 除 KeyUpdate 外，另含 SSL 数据通路 e2e 用例（TLS1.2/1.3 回显、大包跨多记录、单连接多请求复用、并发会话），
# 覆盖 srey SSL 收发路径；socket BIO 与内存 BIO 两种实现按 WITH_SSL_BIO 编译期切换，本测试对两者均适用。
#
# 运行前提：srey 需起 SSL HTTP server（server_http.lua 已加 15443 SSL 监听，证书在 startup.lua 注册）。
# 手动运行：./bin/srey 启动后 python3 bin/py_assist/test_ssl_reneg.py
#
# 说明：Python 标准 ssl 库不暴露 key_update，故本脚本用 ctypes 直调 libssl。
#
# 退出码：0=全部通过，1=有用例失败，2=无法运行（server 未起 / libssl 缺失 / API 不可用）。
import ctypes
import ctypes.util
import os
import select
import socket
import sys
import time

# Windows 控制台默认编码（cp1252/GBK）无法输出中文，会 UnicodeEncodeError；统一转 UTF-8 容错
for _s in (sys.stdout, sys.stderr):
    if hasattr(_s, "reconfigure"):
        try:
            _s.reconfigure(encoding="utf-8", errors="replace")
        except (OSError, ValueError):
            pass

HOST = os.environ.get("SREY_SSL_HOST", "127.0.0.1")
PORT = int(os.environ.get("SREY_SSL_PORT", "15443"))
TIMEOUT = 5.0  # 单次 SSL I/O 等待上限（秒）；超时即判 srey 停滞

# OpenSSL 常量
TLS1_2_VERSION = 0x0303
TLS1_3_VERSION = 0x0304
SSL_CTRL_SET_MIN_PROTO_VERSION = 123
SSL_CTRL_SET_MAX_PROTO_VERSION = 124
SSL_VERIFY_NONE = 0
SSL_KEY_UPDATE_REQUESTED = 1
SSL_ERROR_SSL = 1
SSL_ERROR_WANT_READ = 2
SSL_ERROR_WANT_WRITE = 3
SSL_ERROR_SYSCALL = 5
SSL_ERROR_ZERO_RETURN = 6


class SkipError(Exception):
    # 环境原因无法运行（exit 2），区别于真正的测试失败
    pass


class TLSError(Exception):
    pass


class TLSTimeout(TLSError):
    pass


def _load_one(libname, win_names, unix_names):
    # libname: "ssl" / "crypto"，用于 Windows glob、find_library 回退与报错文案
    if "win32" == sys.platform:
        # CPython 自带 OpenSSL DLL（供 ssl/hashlib 用）在 base_prefix\DLLs；独立安装的在 OpenSSL-Win64\bin。
        # 按全路径加载，ctypes 会到该 DLL 同目录解析依赖项；末尾回退裸名走 PATH 搜索。
        import glob
        candidates = []
        for _d in (os.path.join(sys.base_prefix, "DLLs"),
                   sys.base_prefix,
                   os.path.dirname(sys.executable),
                   r"C:\Program Files\OpenSSL-Win64\bin"):
            candidates += sorted(glob.glob(os.path.join(_d, "lib%s*.dll" % libname)))
        candidates += win_names
    else:
        candidates = list(unix_names)
    found = ctypes.util.find_library(libname)
    if found:
        candidates.append(found)
    for path in candidates:
        try:
            return ctypes.CDLL(path)
        except OSError:
            continue
    raise SkipError("lib%s 未找到（尝试: %s）" % (libname, ", ".join(candidates)))


def _load_libssl():
    # ERR_* 系列符号在 libcrypto 而非 libssl：Windows 的 GetProcAddress 只查本模块导出表，
    # 故须显式加载 libcrypto 才能取到 ERR_*（Unix 上 dlsym 沿依赖链可找到，单独加载仅冗余无害）。
    # 先 libcrypto 后 libssl，让 libssl 的依赖按基名命中已载入模块。
    crypto = _load_one(
        "crypto",
        ["libcrypto-3-x64.dll", "libcrypto-3.dll", "libcrypto-1_1-x64.dll"],
        ["/opt/homebrew/opt/openssl@3/lib/libcrypto.dylib",
         "/usr/local/opt/openssl@3/lib/libcrypto.dylib",
         "libcrypto.so.3", "libcrypto.so", "libcrypto.3.dylib"],
    )
    ssl = _load_one(
        "ssl",
        ["libssl-3-x64.dll", "libssl-3.dll", "libssl-1_1-x64.dll"],
        ["/opt/homebrew/opt/openssl@3/lib/libssl.dylib",
         "/usr/local/opt/openssl@3/lib/libssl.dylib",
         "libssl.so.3", "libssl.so", "libssl.3.dylib"],
    )
    ssl._crypto = crypto
    return ssl


def _setup(f):
    v, i = ctypes.c_void_p, ctypes.c_int

    def sig(name, restype, argtypes, required=True, src=None):
        src = f if src is None else src
        try:
            fn = getattr(src, name)
        except AttributeError:
            if required:
                raise SkipError("OpenSSL 缺少符号 %s" % name)
            return None
        fn.restype = restype
        fn.argtypes = argtypes
        return fn

    f.OPENSSL_init_ssl = sig("OPENSSL_init_ssl", i, [ctypes.c_uint64, v], required=False)
    f.TLS_client_method = sig("TLS_client_method", v, [])
    f.SSL_CTX_new = sig("SSL_CTX_new", v, [v])
    f.SSL_CTX_free = sig("SSL_CTX_free", None, [v])
    f.SSL_CTX_ctrl = sig("SSL_CTX_ctrl", ctypes.c_long, [v, i, ctypes.c_long, v])
    f.SSL_CTX_set_verify = sig("SSL_CTX_set_verify", None, [v, i, v])
    f.SSL_new = sig("SSL_new", v, [v])
    f.SSL_free = sig("SSL_free", None, [v])
    f.SSL_set_fd = sig("SSL_set_fd", i, [v, i])
    f.SSL_connect = sig("SSL_connect", i, [v])
    f.SSL_do_handshake = sig("SSL_do_handshake", i, [v])
    f.SSL_read = sig("SSL_read", i, [v, v, i])
    f.SSL_write = sig("SSL_write", i, [v, v, i])
    f.SSL_get_error = sig("SSL_get_error", i, [v, i])
    f.SSL_get_version = sig("SSL_get_version", ctypes.c_char_p, [v])
    f.SSL_shutdown = sig("SSL_shutdown", i, [v])
    f.SSL_key_update = sig("SSL_key_update", i, [v, i], required=False)
    f.ERR_get_error = sig("ERR_get_error", ctypes.c_ulong, [], src=f._crypto)
    f.ERR_error_string_n = sig("ERR_error_string_n", None,
                               [ctypes.c_ulong, v, ctypes.c_size_t], src=f._crypto)
    if f.OPENSSL_init_ssl:
        f.OPENSSL_init_ssl(0, None)
    return f


_LIB = None


def lib():
    global _LIB
    if _LIB is None:
        _LIB = _setup(_load_libssl())
    return _LIB


def _err_stack():
    f = lib()
    msgs = []
    while True:
        e = f.ERR_get_error()
        if 0 == e:
            break
        buf = ctypes.create_string_buffer(256)
        f.ERR_error_string_n(e, buf, len(buf))
        msgs.append(buf.value.decode("latin1"))
    return "; ".join(msgs) if msgs else "(no err)"


class TLSClient:
    def __init__(self, host, port, ver):
        self.host = host
        self.port = port
        self.ver = ver  # TLS1_2_VERSION / TLS1_3_VERSION
        self.sock = None
        self.ctx = None
        self.ssl = None

    def connect(self):
        f = lib()
        self.ctx = f.SSL_CTX_new(f.TLS_client_method())
        if not self.ctx:
            raise TLSError("SSL_CTX_new: " + _err_stack())
        f.SSL_CTX_set_verify(self.ctx, SSL_VERIFY_NONE, None)
        # 锁定协议版本上下限到目标版本，确保测的就是 1.2 / 1.3
        f.SSL_CTX_ctrl(self.ctx, SSL_CTRL_SET_MIN_PROTO_VERSION, self.ver, None)
        f.SSL_CTX_ctrl(self.ctx, SSL_CTRL_SET_MAX_PROTO_VERSION, self.ver, None)
        try:
            self.sock = socket.create_connection((self.host, self.port), TIMEOUT)
        except OSError as e:
            raise SkipError("连接 %s:%d 失败（srey SSL server 未起？）: %s"
                            % (self.host, self.port, e))
        self.sock.setblocking(False)
        self.ssl = f.SSL_new(self.ctx)
        if not self.ssl:
            raise TLSError("SSL_new: " + _err_stack())
        f.SSL_set_fd(self.ssl, self.sock.fileno())  # BIO_NOCLOSE，fd 由 Python sock 关
        self._op(lambda: f.SSL_connect(self.ssl), "SSL_connect")

    def _wait(self, deadline, want_read, want_write):
        remain = deadline - time.monotonic()
        if remain <= 0:
            raise TLSTimeout("等待%s超时（疑似 srey 停滞）" % ("可读" if want_read else "可写"))
        r = [self.sock] if want_read else []
        w = [self.sock] if want_write else []
        rl, wl, _ = select.select(r, w, [], remain)
        if not rl and not wl:
            raise TLSTimeout("select 超时（疑似 srey 停滞/忙转未推进）")

    def _op(self, call, what):
        # select 驱动一次 SSL 操作至完成；返回 ret>0
        f = lib()
        deadline = time.monotonic() + TIMEOUT
        while True:
            ret = call()
            if ret > 0:
                return ret
            err = f.SSL_get_error(self.ssl, ret)
            if err == SSL_ERROR_WANT_READ:
                self._wait(deadline, True, False)
            elif err == SSL_ERROR_WANT_WRITE:
                self._wait(deadline, False, True)
            elif err == SSL_ERROR_ZERO_RETURN:
                raise TLSError("%s: 对端关闭连接" % what)
            else:
                raise TLSError("%s err=%d: %s" % (what, err, _err_stack()))

    def write(self, data):
        f = lib()
        sent, n = 0, len(data)
        while sent < n:
            chunk = data[sent:]
            buf = ctypes.create_string_buffer(chunk, len(chunk))
            # 同一 buf/len 用于 WANT_* 重试，满足 OpenSSL 重试契约
            sent += self._op(lambda: f.SSL_write(self.ssl, buf, len(chunk)), "SSL_write")

    def read(self, n):
        f = lib()
        buf = ctypes.create_string_buffer(n)
        ret = self._op(lambda: f.SSL_read(self.ssl, buf, n), "SSL_read")
        return buf.raw[:ret]

    def key_update(self):
        f = lib()
        if not f.SSL_key_update:
            raise SkipError("libssl 无 SSL_key_update")
        if 1 != f.SSL_key_update(self.ssl, SSL_KEY_UPDATE_REQUESTED):
            raise TLSError("SSL_key_update: " + _err_stack())
        self._op(lambda: f.SSL_do_handshake(self.ssl), "do_handshake(keyupdate)")

    def version(self):
        f = lib()
        v = f.SSL_get_version(self.ssl)
        return v.decode("ascii") if v else "?"

    def close(self):
        f = lib()
        if self.ssl:
            try:
                f.SSL_shutdown(self.ssl)
            except Exception:
                pass
            f.SSL_free(self.ssl)
            self.ssl = None
        if self.ctx:
            f.SSL_CTX_free(self.ctx)
            self.ctx = None
        if self.sock:
            self.sock.close()
            self.sock = None


def _read_response(c):
    # 按 Content-Length / chunked 精确读完一个 HTTP 响应，不多读（保持连接静默以便干净重协商）
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += c.read(4096)
        if len(buf) > 8 * 1024 * 1024:
            raise TLSError("响应头过大")
    head, _, body = buf.partition(b"\r\n\r\n")
    cl, chunked = 0, False
    for line in head.split(b"\r\n"):
        low = line.lower()
        if low.startswith(b"content-length:"):
            cl = int(line.split(b":", 1)[1].strip())
        elif low.startswith(b"transfer-encoding:") and b"chunked" in low:
            chunked = True
    if chunked:
        while not body.endswith(b"0\r\n\r\n"):
            body += c.read(4096)
    else:
        while len(body) < cl:
            body += c.read(min(65536, cl - len(body)))
    return head, body


def _http_get(c, path="/"):
    c.write(("GET %s HTTP/1.1\r\nHost: srey\r\nConnection: keep-alive\r\n\r\n" % path).encode())
    return _read_response(c)


def _http_post(c, body, path="/"):
    head = ("POST %s HTTP/1.1\r\nHost: srey\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n"
            % (path, len(body))).encode()
    c.write(head + body)
    return _read_response(c)


def _assert200(head):
    if b" 200 " not in head:
        raise TLSError("响应非 200: %r" % head[:80])


def _echo_check(c, body, path="/"):
    # POST 一段 body 并校验服务端原样回显（字节级一致）
    h, b = _http_post(c, body, path)
    _assert200(h)
    if b != body:
        raise TLSError("回显不一致: 期望 %d 字节, 实得 %d" % (len(body), len(b)))


# ---- 用例 ----

def case_baseline_keepalive():
    # 对照组：同连接两次请求、不重协商，排除 HTTP keep-alive 本身的干扰
    c = TLSClient(HOST, PORT, TLS1_2_VERSION)
    try:
        c.connect()
        _assert200(_http_get(c)[0])
        _assert200(_http_get(c)[0])
        return True
    finally:
        c.close()


def case_tls13_key_update():
    # 方向 A 主测：TLS1.3 KeyUpdate，srey SSL_read 收到要写响应 KeyUpdate
    c = TLSClient(HOST, PORT, TLS1_3_VERSION)
    try:
        c.connect()
        if "TLSv1.3" != c.version():
            raise SkipError("未协商到 TLS1.3（实际 %s）" % c.version())
        _assert200(_http_get(c)[0])
        c.key_update()
        _assert200(_http_get(c)[0])
        big = b"y" * (16 * 1024)                    # < MAX_PACK_SIZE(65535)，避免 PACK_TOO_LONG
        h, b = _http_post(c, big)
        _assert200(h)
        if b != big:
            raise TLSError("大 body 回显不一致 (got %d)" % len(b))
        return True
    finally:
        c.close()


def case_tls12_post_echo():
    # TLS1.2 数据期 POST 回显：覆盖 1.2 在 srey SSL 收发路径（baseline 仅 GET 小包）
    c = TLSClient(HOST, PORT, TLS1_2_VERSION)
    try:
        c.connect()
        _echo_check(c, b"hello srey ssl 1.2")
        _echo_check(c, os.urandom(4096))
        return True
    finally:
        c.close()


def case_large_multirecord_echo():
    # 大 body 跨多条 TLS 记录（单记录上限 16KB）：压发送侧 buf_w 暂存/背压 + 接收侧抽干 rbio 多记录重组
    c = TLSClient(HOST, PORT, TLS1_3_VERSION)
    try:
        c.connect()
        big = os.urandom(60000)                     # < MAX_PACK_SIZE(65535)，约 4 条 TLS 记录
        _echo_check(c, big)
        return True
    finally:
        c.close()


def case_many_sequential():
    # 同连接多次往返：压反复 加密/解密 + buf_w 每轮归零，验证无残留/串包
    c = TLSClient(HOST, PORT, TLS1_3_VERSION)
    try:
        c.connect()
        for i in range(50):
            _echo_check(c, ("seq-%d-" % i).encode() + os.urandom(200))
        return True
    finally:
        c.close()


def case_concurrent_sessions():
    # 多条 SSL 连接同时存活后逐一回显：验证每连接 rbio/wbio 隔离，会话间互不串扰
    clients = [TLSClient(HOST, PORT, TLS1_3_VERSION) for _ in range(6)]
    try:
        for c in clients:
            c.connect()
        for i, c in enumerate(clients):
            _echo_check(c, ("conn-%d-" % i).encode() + os.urandom(1500))
        return True
    finally:
        for c in clients:
            c.close()


def case_key_update_repeated():
    # 连续多次 KeyUpdate：压 KeyUpdate 状态反复 set/clear（IOCP 的 KEYUPDATE/NORECV/探针、
    # usock 的 KEYUPDATE/EVENT_WRITE），验证无残留、每次后连接仍正常收发
    c = TLSClient(HOST, PORT, TLS1_3_VERSION)
    try:
        c.connect()
        if "TLSv1.3" != c.version():
            raise SkipError("未协商到 TLS1.3（实际 %s）" % c.version())
        for i in range(8):
            c.key_update()
            _echo_check(c, ("ku-%d-" % i).encode() + os.urandom(300))
        return True
    finally:
        c.close()


def case_key_update_then_large_echo():
    # KeyUpdate 紧接大包回显（约 4 条 TLS 记录）：覆盖 KeyUpdate 与业务大数据收发交织——
    # IOCP 侧 KeyUpdate 冲完后 SENDING 接力抽积压 buf_s，usock 侧 KEYUPDATE 与 EVENT_WRITE 协同
    c = TLSClient(HOST, PORT, TLS1_3_VERSION)
    try:
        c.connect()
        if "TLSv1.3" != c.version():
            raise SkipError("未协商到 TLS1.3（实际 %s）" % c.version())
        c.key_update()
        _echo_check(c, os.urandom(60000))           # < MAX_PACK_SIZE(65535)
        return True
    finally:
        c.close()


def case_key_update_close():
    # KeyUpdate 后不等响应立即关，再开新连接验证 server 健康：
    # 覆盖 KeyUpdate 处理与连接回收叠加（IOCP send_close 经 NORECV 直接回收 / usock close）不崩不泄漏
    c = TLSClient(HOST, PORT, TLS1_3_VERSION)
    try:
        c.connect()
        if "TLSv1.3" != c.version():
            raise SkipError("未协商到 TLS1.3（实际 %s）" % c.version())
        c.key_update()
        c.write(b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n")  # 触发 server SSL_read 处理 KeyUpdate
    finally:
        c.close()                                              # 不等响应立即关
    c2 = TLSClient(HOST, PORT, TLS1_3_VERSION)
    try:
        c2.connect()
        _assert200(_http_get(c2)[0])
        return True
    finally:
        c2.close()


def _read_resp_with_keyupdate(c, step=150000):
    # 读 HTTP 响应；body 每读够 step 字节就触发一次客户端 KeyUpdate，使 KeyUpdate 记录在
    # 「服务端仍在发送 body」时到达——命中 IOCP _olp_tcp_recv 的 !SENDING 守卫，buf_s 排空时
    # 由 _olp_tcp_send 接力探针冲刷挂起的 KeyUpdate（overlap.c）。按字节而非 read 次数控频，
    # 因 SSL_read 一次仅返回一条 record（~16KB），且需避免 OpenSSL 对端 KeyUpdate 速率限制。
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += c.read(4096)
    head, _, body = buf.partition(b"\r\n\r\n")
    cl = 0
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            cl = int(line.split(b":", 1)[1].strip())
    next_ku = step
    while len(body) < cl:
        body += c.read(min(65536, cl - len(body)))
        if len(body) >= next_ku and len(body) < cl:
            c.key_update()
            next_ku += step
    return head, body


def case_key_update_during_server_send():
    # 发侧接力主测：GET /down 让服务端回大 body（in-flight send，SENDING 置位），
    # 读 body 期间穿插 KeyUpdate，命中 IOCP overlap.c 数据发送中的 KeyUpdate 发侧接力。
    # 仅 IOCP/Windows 真测该修复；usock 无此守卫，此处验「KeyUpdate 撞下行不破坏」。
    EXPECT = 760000                                  # 与 server_http.lua _DOWN_BODY 一致
    c = TLSClient(HOST, PORT, TLS1_3_VERSION)
    try:
        c.connect()
        if "TLSv1.3" != c.version():
            raise SkipError("未协商到 TLS1.3（实际 %s）" % c.version())
        c.write(b"GET /down HTTP/1.1\r\nHost: srey\r\nConnection: keep-alive\r\n\r\n")
        head, body = _read_resp_with_keyupdate(c)
        _assert200(head)
        if len(body) != EXPECT:
            raise TLSError("下行长度不符: 期望 %d 实得 %d（疑似发送中 KeyUpdate 停滞/串包）"
                           % (EXPECT, len(body)))
        if body != b"D" * EXPECT:
            raise TLSError("下行内容损坏（疑似 KeyUpdate 串包）")
        _assert200(_http_get(c)[0])                  # KeyUpdate 后连接仍健康
        return True
    finally:
        c.close()


CASES = [
    ("baseline keep-alive", case_baseline_keepalive),
    ("TLS1.3 KeyUpdate (dir-A)", case_tls13_key_update),
    ("TLS1.2 POST echo", case_tls12_post_echo),
    ("large multi-record echo", case_large_multirecord_echo),
    ("many sequential echo", case_many_sequential),
    ("concurrent SSL sessions", case_concurrent_sessions),
    ("repeated KeyUpdate", case_key_update_repeated),
    ("KeyUpdate then large echo", case_key_update_then_large_echo),
    ("KeyUpdate then immediate close", case_key_update_close),
    ("KeyUpdate during server send (dir-A in-flight)", case_key_update_during_server_send),
]


def main():
    try:
        lib()  # 预加载 libssl，失败则整体 skip
    except SkipError as e:
        print("[ssl_reneg] SKIP: %s" % e, flush=True)
        return 2
    fails = skips = 0
    for name, fn in CASES:
        try:
            fn()
        except SkipError as e:
            print("[ssl_reneg] %s: SKIP %s" % (name, e), flush=True)
            skips += 1
            continue
        except Exception as e:
            print("[ssl_reneg] %s: FAIL %s" % (name, e), flush=True)
            fails += 1
            continue
        print("[ssl_reneg] %s: PASS" % name, flush=True)
    total = len(CASES)
    print("[ssl_reneg] summary: %d passed, %d failed, %d skipped (of %d)"
          % (total - fails - skips, fails, skips, total), flush=True)
    if fails > 0:
        return 1
    if skips == total:  # 全部 skip（多半 server 未起）视为无法运行
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
