#!/usr/bin/env python3
"""Windows minidump 离线解析 —— 不依赖 WinDbg/dbghelp，可在 macOS/Linux 上读 .dmp。

用法:  python3 tools/mdmp.py <file.dmp> [--strings] [--scan-all]

给出:  异常码与出错地址(映射到 模块+偏移)、系统与模块表、全部线程的 RIP/RSP、
       崩溃线程寄存器、栈的保守扫描、以及自动识别的递归环(反复出现的返回地址 + 层间步长)。

拿到 模块+偏移 之后在 Windows 上解符号:
    windbg:  ln srey+0x224d52
    或:      dumpbin /disasm /symbols bin\\srey.exe  (需要 /Zi 编出的 PDB 才有行号)

坑: MINIDUMP_THREAD 末尾的 ThreadContext 是 {DataSize, Rva} 两个 uint32。
    只取一个字段的话每个线程都会拿同一个偏移去读寄存器, 结果所有线程 RIP/RSP 完全相同
    —— 看起来像"全部线程卡在同一处", 实际是解析错了。
"""
import collections
import re
import struct
import sys

EXC = {
    0xC0000005: "ACCESS_VIOLATION",
    0xC00000FD: "STACK_OVERFLOW",
    0xC000001D: "ILLEGAL_INSTRUCTION",
    0xC0000025: "NONCONTINUABLE_EXCEPTION",
    0xC0000094: "INT_DIVIDE_BY_ZERO",
    0xC0000096: "PRIV_INSTRUCTION",
    0x80000003: "BREAKPOINT",
    0xC0000374: "HEAP_CORRUPTION",
    0xC0000409: "STACK_BUFFER_OVERRUN / __fastfail",
    0xC0000602: "FAIL_FAST_EXCEPTION",
    0xE06D7363: "C++ EH exception (throw)",
}
STREAM = {3: "ThreadList", 4: "ModuleList", 5: "MemoryList", 6: "Exception",
          7: "SystemInfo", 8: "ThreadExList", 9: "Memory64List", 15: "MiscInfo",
          16: "MemoryInfoList", 17: "ThreadInfoList"}
ARCH = {0: "x86", 5: "ARM", 6: "IA64", 9: "x64", 12: "ARM64"}
# CONTEXT_AMD64 里各寄存器相对结构体起始的偏移
CTX = {"Rax": 0x78, "Rcx": 0x80, "Rdx": 0x88, "Rbx": 0x90, "Rsp": 0x98, "Rbp": 0xA0,
       "Rsi": 0xA8, "Rdi": 0xB0, "R8": 0xB8, "R9": 0xC0, "R10": 0xC8, "R11": 0xD0,
       "R12": 0xD8, "R13": 0xE0, "R14": 0xE8, "R15": 0xF0, "Rip": 0xF8}
FILL = (0xCCCCCCCCCCCCCCCC, 0, 0xFFFFFFFFFFFFFFFF, 0xDDDDDDDDDDDDDDDD)


class Dump:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        sig, self.ver, nstream, dirrva = struct.unpack_from("<IIII", self.d, 0)
        if sig != 0x504D444D:
            raise SystemExit("不是 minidump (缺少 MDMP 魔数)")
        self.streams = {}
        for i in range(nstream):
            st, sz, rva = struct.unpack_from("<III", self.d, dirrva + i * 12)
            self.streams[st] = (sz, rva)
        self.mods = self._modules()
        self.regions = self._memory()

    def mdstring(self, rva):
        ln, = struct.unpack_from("<I", self.d, rva)
        return self.d[rva + 4:rva + 4 + ln].decode("utf-16-le", "replace")

    def _modules(self):
        out = []
        if 4 not in self.streams:
            return out
        _, r = self.streams[4]
        n, = struct.unpack_from("<I", self.d, r)
        for i in range(n):
            base, size, _ck, _ts, nrva = struct.unpack_from("<QIIII", self.d, r + 4 + i * 108)
            out.append((base, size, self.mdstring(nrva)))
        out.sort()
        return out

    def _memory(self):
        out = []
        if 5 in self.streams:
            _, r = self.streams[5]
            n, = struct.unpack_from("<I", self.d, r)
            for i in range(n):
                s, sz, rva = struct.unpack_from("<QII", self.d, r + 4 + i * 16)
                out.append((s, sz, rva))
        if 9 in self.streams:
            _, r = self.streams[9]
            n, baserva = struct.unpack_from("<QQ", self.d, r)
            off = baserva
            for i in range(n):
                s, sz = struct.unpack_from("<QQ", self.d, r + 16 + i * 16)
                out.append((s, sz, off))
                off += sz
        out.sort()
        return out

    def whose(self, addr):
        for base, size, name in self.mods:
            if base <= addr < base + size:
                return f"{name.rsplit(chr(92), 1)[-1]}+0x{addr - base:x}"
        return None

    def readmem(self, addr, ln):
        for s, sz, rva in self.regions:
            if s <= addr < s + sz:
                o = rva + (addr - s)
                return self.d[o:min(o + ln, rva + sz)]
        return None

    def threads(self):
        out = []
        if 3 not in self.streams:
            return out
        _, r = self.streams[3]
        n, = struct.unpack_from("<I", self.d, r)
        for i in range(n):
            o = r + 4 + i * 48
            # 末尾四个 uint32 依次是 栈DataSize/栈Rva/上下文DataSize/上下文Rva —— 别少取字段
            tid, _su, _pc, _pr, teb, ss, ssz, srva, _csz, crva = \
                struct.unpack_from("<IIIIQQIIII", self.d, o)
            out.append(dict(tid=tid, teb=teb, stack=(ss, ssz, srva), ctx=crva))
        return out

    # 只认 CONTEXT_AMD64 的偏移; 别的架构由调用方按 SystemInfo 的 arch 先挡掉
    def regs(self, crva):
        return {k: struct.unpack_from("<Q", self.d, crva + o)[0] for k, o in CTX.items()}


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    dm = Dump(sys.argv[1])
    want_strings = "--strings" in sys.argv
    scan_all = "--scan-all" in sys.argv

    print("== 流 ==")
    print("  " + ", ".join(f"{STREAM.get(k, k)}({v[0]}B)" for k, v in sorted(dm.streams.items())))

    # 没有 SystemInfo 流时按 AMD64 处理(沿用旧行为); 有则据实判定
    x64 = True
    if 7 in dm.streams:
        _, r = dm.streams[7]
        arch, plvl, prev, ncpu, _pt, major, minor, build = struct.unpack_from("<HHHBBIII", dm.d, r)
        x64 = (9 == arch)
        print(f"\n== 系统 ==\n  {ARCH.get(arch, arch)}  {major}.{minor}.{build}  cpu={ncpu}"
              f"  family={plvl} model=0x{prev:04x}")
        if not x64:
            print(f"  注意: 寄存器只按 CONTEXT_AMD64 的偏移解析, {ARCH.get(arch, arch)} 的 CONTEXT"
                  " 布局不同, 下面的 rip/rsp 与寄存器表已跳过(打出来会是别的寄存器的值)")
        emu = [n for _b, _s, n in dm.mods if "xtajit" in n.lower()]
        if emu:
            print("  注意: 加载了 xtajit* —— 这是 ARM64 上跑 x64 的模拟层, 栈帧与原生不同")

    faultid, faultaddr = None, None
    if 6 in dm.streams:
        _, r = dm.streams[6]
        faultid, = struct.unpack_from("<I", dm.d, r)
        code, eflags, nested, faultaddr, nparam, _u = struct.unpack_from("<IIQQII", dm.d, r + 8)
        params = struct.unpack_from("<15Q", dm.d, r + 8 + 32)
        print(f"\n== 异常 ==\n  code   0x{code:08X}  {EXC.get(code, '?')}")
        print(f"  地址   0x{faultaddr:016X}   {dm.whose(faultaddr) or '<不在任何已加载模块内>'}")
        print(f"  线程   {faultid}  flags=0x{eflags:x}  nested=0x{nested:x}")
        if code in (0xC0000005, 0xC00000FD) and nparam >= 2:
            op = {0: "读", 1: "写", 8: "执行(DEP)"}.get(params[0], params[0])
            print(f"  访问   {op} 0x{params[1]:016X}")
        elif nparam:
            print("  参数   " + " ".join(f"0x{p:x}" for p in params[:nparam]))

    ths = dm.threads()
    print(f"\n== 线程 {len(ths)} 个 ==")
    for t in ths:
        ss, ssz, _ = t["stack"]
        mark = "  <== 崩溃线程" if t["tid"] == faultid else ""
        if not x64:
            print(f"  tid {t['tid']:<6} 已用栈 {ssz // 1024}KB{mark}")
            continue
        g = dm.regs(t["ctx"])
        print(f"  tid {t['tid']:<6} rip {g['Rip']:016X} {dm.whose(g['Rip']) or '?':<32}"
              f" rsp {g['Rsp']:016X} 已用栈 {ssz // 1024}KB{mark}")

    # hang dump(任务管理器抓的 / procdump -ma)没有异常流,自然也没有崩溃线程,
    # 但下面的模块表与 --strings 不依赖它,不能一起跳过 —— 死锁排查要的正是模块+偏移
    ft = next((t for t in ths if t["tid"] == faultid), None)
    if ft is not None:
        ss, ssz, srva = ft["stack"]
        if x64:
            g = dm.regs(ft["ctx"])
            print(f"\n== 崩溃线程 {ft['tid']} 寄存器 ==")
            ks = list(CTX)
            for i in range(0, len(ks), 4):
                print("  " + "  ".join(f"{k:<4}{g[k]:016X}" for k in ks[i:i + 4]))
        print(f"\n  捕获栈 0x{ss:X}..0x{ss + ssz:X}  ({ssz} 字节 = {ssz // 1024}KB)")

        blob = dm.d[srva:srva + ssz]
        stacklo, stackhi = ss, ss + ssz

        # 栈上所有落在模块里的 qword —— 保守回溯(含误报, 但不会漏)
        cand = []
        for off in range(0, len(blob) - 7, 8):
            v, = struct.unpack_from("<Q", blob, off)
            w = dm.whose(v)
            if w:
                cand.append((off, v, w))
        bymod = collections.Counter(w.split("+")[0] for _o, _v, w in cand)
        print(f"\n== 保守栈扫描: {len(cand)} 个候选返回地址 ==")
        for m, c in bymod.most_common():
            print(f"    {m:<28} {c}")

        print("\n  自 RSP 起最近的 30 个（最内层调用在前）:")
        for off, v, w in cand[:30]:
            print(f"    +0x{off:05x}  0x{v:016X}  {w}")

        # 自动识别递归环: 同一返回地址反复出现且间距规整
        hot = collections.Counter(v for _o, v, _w in cand).most_common(12)
        cyc = [(v, c) for v, c in hot if c >= 20]
        if cyc:
            print(f"\n== 疑似递归环 ==")
            for v, c in cyc:
                offs = [o for o, vv, _w in cand if vv == v]
                steps = collections.Counter(offs[i + 1] - offs[i] for i in range(len(offs) - 1))
                stride, hits = steps.most_common(1)[0]
                print(f"  {dm.whose(v):<30} {c:>5} 次   层间步长 {stride} 字节"
                      f" ({hits}/{len(offs) - 1} 一致)")
            top = cyc[0]
            offs = [o for o, vv, _w in cand if vv == top[0]]
            span = offs[-1] - offs[0]
            print(f"\n  递归占据 {span} 字节 ({span // 1024}KB), 约 {top[1]} 层")
            print(f"  每层约 {span / max(top[1] - 1, 1):.0f} 字节  ->  "
                  f"1MB 栈最多 {1048576 // max(int(span / max(top[1] - 1, 1)), 1)} 层, "
                  f"512KB 栈最多 {524288 // max(int(span / max(top[1] - 1, 1)), 1)} 层")
            print("  环上这几个地址就是互相递归的函数, 在 Windows 上用 ln 解符号")

            # 逐层比对同一槽位: 恒定的是 ctx/doc 之类的共享对象, 递变的是每层自己的数据
            print(f"\n== 层间槽位比对 (恒定 = 每层共用同一对象; 递变 = 每层各自的数据) ==")
            anchor = offs[len(offs) // 6: len(offs) * 5 // 6]
            slots = collections.defaultdict(list)
            for a in anchor:
                for k in range(0, min(int(span / max(top[1] - 1, 1)), 0x600), 8):
                    p = a + k
                    if 0 <= p <= len(blob) - 8:
                        slots[k].append(struct.unpack_from("<Q", blob, p)[0])
            for k in sorted(slots):
                vs = slots[k]
                u = set(vs)
                if len(u) == 1 and vs[0] not in FILL:
                    note = dm.whose(vs[0]) or ("<栈内>" if stacklo <= vs[0] < stackhi else "")
                    print(f"  {k:+#06x}  恒定 0x{vs[0]:016X}  {note}")
                elif len(u) == len(vs) and not (u & set(FILL)) and len(vs) > 3:
                    step = vs[1] - vs[0]
                    if step and all(vs[i + 1] - vs[i] == step for i in range(min(6, len(vs) - 1))):
                        print(f"  {k:+#06x}  等差递变 步长 {step:+#x}  样本 0x{vs[0]:016X}")

    if want_strings:
        print("\n== 抓取内存中的字符串 (>=6 可打印字符, 按出现次数) ==")
        cnt = collections.Counter()
        src = dm.regions if scan_all else [r for r in dm.regions if r[1] < 0x100000]
        for _s, sz, rva in src:
            for m in re.finditer(rb"[ -~]{6,}", dm.d[rva:rva + sz]):
                cnt[m.group().decode()] += 1
        for t, c in cnt.most_common(60):
            print(f"  {c:>4}x  {t[:110]}")

    print(f"\n== 模块 {len(dm.mods)} 个 ==")
    for base, size, name in dm.mods:
        print(f"  {base:016X} +{size:<9x} {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
