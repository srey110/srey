#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""检查二进制里有没有绕过 TLS_DEFINE 的线程局部变量访问。

协程会在别的 worker 线程上恢复，而编译器会把 TLS 地址复用到挂起之后(理由见
lib/base/macro_util.h 的 TLS_DEFINE)。修好后全仓的 TLS 访问只该出现在三处：TLS_DEFINE
生成的 *_addr_ 访问器、minicoro 自己那几个已防住的函数、只做原子加的 memory.c `_slot`。
本脚本把每次 TLS 访问解析到具体变量，别处出现即报出来。

支持 arm64 上本项目用到的两种格式：
    Mach-O(macOS)    adrp + add 指向 __thread_vars 描述符
    ELF(Linux/FreeBSD aarch64，clang 与 gcc 产物) mrs TPIDR_EL0 起算，按 TP + 0x10 + st_value 对 TLS 符号
其余格式(x86-64、32 位、PE、通用二进制)以及一处 TLS 访问都认不出的产物(被 strip 等)报无法检查，退出码 2。

用法:
    python3 tools/tls_check.py bin/test [bin/srey ...]
有违规退出码 1，否则 0。须在编完的 -O2 -flto 产物上跑，debug 构建不内联测不出来。
Mach-O 按描述符逐个认，结果精确；ELF 靠反汇编线性扫描，只报明确解析到的违规，
解析不了的与外部库 TLS 只打 warning(不计违规)，新增的裸写靠 mk.sh 的源码门禁兜住。
"""
import re
import shutil
import subprocess
import sys

# 放行的变量：访问只有原子加，读到别的线程那份也不出错
ALLOW_VARS = {"_slot"}
# 放行的函数：TLS_DEFINE 的访问器；minicoro 自己的 TLS 已逐个核过不跨切栈
ALLOW_FUNCS = re.compile(r"(_addr_|^_?_mco_running|^_?_mco_set_current|^_?_mco_jump(in|out)|"
                         r"^_?_mco_asan_\w*|^_?mco_(resume|yield|thread_cleanup))(\.\S*)?$")
# ELF aarch64 的 TCB 大小：变量地址 = TP + 0x10 + st_value
ELF_TCB = 0x10
# ELF 上低位偏移只在高位 add 之后这么多条指令内认(理由见 _scan_elf)
ELF_WINDOW = 24


def _run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def _objdump():
    if shutil.which("xcrun"):
        return ["xcrun", "llvm-objdump"]
    if shutil.which("llvm-objdump"):
        return ["llvm-objdump"]
    return ["objdump"]


def _functions(path):
    # 逐函数切开反汇编：返回 [(函数名, [指令文本...])]
    out = _run(_objdump() + ["-d", "--no-show-raw-insn", path])
    funcs = []
    for line in out.split("\n"):
        m = re.match(r"^[0-9a-f]+ <([^>]+)>:$", line)
        if m:
            funcs.append((m.group(1), []))
            continue
        if funcs and ":" in line:
            funcs[-1][1].append(line.split(":", 1)[1].strip().lower())
    return funcs


def _macho_vars(path):
    # __thread_vars 描述符地址 -> 变量名
    tv = {}
    for line in _run(["nm", "-m", path]).split("\n"):
        m = re.match(r"^([0-9a-f]+) \(__DATA,__thread_vars\) \S+ (\S+)", line)
        if m:
            # Mach-O 给 C 符号多加一个前导下划线
            sym = m.group(2)
            tv[int(m.group(1), 16)] = sym[1:] if sym.startswith("_") else sym
    return tv


def _elf_vars(path):
    # [(起始偏移, 大小, 变量名)]，偏移已换算成相对 TP。跳过零长度的标记符号($d、_TLS_MODULE_BASE_)，
    # gcc LTO 给静态变量加的 .lto_priv.N 后缀去掉(C 标识符里不会有 '.')
    res = []
    for line in _run(["readelf", "-sW", path]).split("\n"):
        f = line.split()
        if len(f) >= 8 and f[3] == "TLS" and f[6] != "UND" and f[2].isdigit() and int(f[2]) > 0:
            res.append((ELF_TCB + int(f[1], 16), int(f[2]), f[7].split(".")[0]))
    return res


def _elf_name(tab, off):
    for start, size, name in tab:
        if start <= off < start + size:
            return name
    return "?+%#x" % off


def _scan_macho(funcs, tv):
    bad = {}
    hit = set()
    for name, ins in funcs:
        page = {}
        seen = set()
        for i in ins:
            m = re.search(r"\badrp\s+(x\d+),\s*0x([0-9a-f]+)", i)
            if m:
                page[m.group(1)] = int(m.group(2), 16)
                continue
            m = re.search(r"\badd\s+(x\d+),\s*(x\d+),\s*#0x([0-9a-f]+)", i)
            if m and m.group(2) in page:
                addr = page[m.group(2)] + int(m.group(3), 16)
                if addr in tv:
                    seen.add(tv[addr])
        hit |= seen
        if ALLOW_FUNCS.search(name):
            continue
        # 访问器之外碰 *_raw_ 也算违规：只该在 *_addr_ 里取它
        vars_bad = {v for v in seen if v not in ALLOW_VARS}
        if vars_bad:
            bad[name] = sorted(vars_bad)
    return bad, hit


def _scan_elf(funcs, tab):
    # 本项目的变量都在可执行文件里，走 local-exec：mrs 取 TP → add #hi, lsl #12 → add #lo 或 [reg, #lo]。
    # gcc 还会先 add 到一个段锚点再 [reg, #off] 落到真正的变量，所以低位 add 之后接着跟：被当基址解引用的
    # 按解引用的偏移认，没被解引用就流走的(地址传出去)才按它自己认。低位只在前一步之后 ELF_WINDOW 条指令内认：
    # 线性扫描不分基本块，窗口外寄存器多半已改作他用，认了反而报错。
    # TP 加寄存器偏移的是外部库的 TLS(initial-exec / TLSDESC，如 FreeBSD ctype 宏读的 _ThreadRuneLocale)
    bad = {}
    warn = {}
    hit = set()
    imm = r"#(0x[0-9a-f]+|\d+)"
    for name, ins in funcs:
        if not any("tpidr_el0" in i for i in ins):
            continue
        reg = {}# 寄存器 -> (相对 TP 的偏移, 出现位置)
        pend = {}# 低位 add 得到、还没被当基址用过的寄存器 -> 偏移
        spill = {}
        seen = set()
        extern = False

        def drop(r):
            if r in pend:
                seen.add(_elf_name(tab, pend.pop(r)))
            reg.pop(r, None)

        for idx, i in enumerate(ins):
            op = i.split()[0] if i.split() else ""
            if op in ("b", "br", "ret"):
                # 无条件跳转之后的下一条只能从别处跳进来，寄存器里是什么线性扫描看不出；
                # 只留 x19~x28 里的原始 TP：编译器把它提到函数入口一直用(修复前的 bug 正是这个形状)
                for r in list(pend):
                    drop(r)
                keep = {r: v for r, v in reg.items() if 0 == v[0] and 19 <= int(r[1:]) <= 28}
                reg.clear()
                reg.update(keep)
                continue
            m = re.match(r"mrs\s+(x\d+),\s*tpidr_el0", i)
            if m:
                drop(m.group(1))
                reg[m.group(1)] = (0, idx)
                continue
            m = re.match(r"add\s+(x\d+),\s*(x\d+),\s*" + imm + r"(,\s*lsl #12)?", i)
            if m and m.group(2) in reg:
                base, at = reg[m.group(2)]
                val = int(m.group(3), 0) << (12 if m.group(4) else 0)
                pend.pop(m.group(2), None)
                if m.group(1) != m.group(2):
                    drop(m.group(1))
                if m.group(4) or idx - at <= ELF_WINDOW:
                    reg[m.group(1)] = (base + val, idx)
                    if not m.group(4):
                        pend[m.group(1)] = base + val
                else:
                    reg.pop(m.group(1), None)
                continue
            m = re.match(r"(stp|str)\s+(x\d+)(?:,\s*(x\d+))?,\s*\[sp(?:,\s*" + imm + r")?\]", i)
            if m:
                off = int(m.group(4), 0) if m.group(4) else 0
                for k, r in enumerate([m.group(2), m.group(3)] if m.group(1) == "stp" else [m.group(2)]):
                    if r in reg:
                        spill[off + 8 * k] = reg[r][0]
                    else:
                        spill.pop(off + 8 * k, None)
                continue
            m = re.match(r"(ldp|ldr)\s+(x\d+)(?:,\s*(x\d+))?,\s*\[sp(?:,\s*" + imm + r")?\]", i)
            if m:
                off = int(m.group(4), 0) if m.group(4) else 0
                for k, r in enumerate([m.group(2), m.group(3)] if m.group(1) == "ldp" else [m.group(2)]):
                    drop(r)
                    if off + 8 * k in spill:
                        reg[r] = (spill[off + 8 * k], idx)
                continue
            m = re.search(r"\[(x\d+)(?:,\s*" + imm + r")?\]", i)
            if m and m.group(1) in reg:
                pend.pop(m.group(1), None)
                if idx - reg[m.group(1)][1] <= ELF_WINDOW:
                    seen.add(_elf_name(tab, reg[m.group(1)][0] + (int(m.group(2), 0) if m.group(2) else 0)))
            m = re.search(r"\[(x\d+),\s*x\d+", i)
            if m and m.group(1) in reg:
                extern = True
            m = re.match(r"add\s+x\d+,\s*(x\d+),\s*x\d+", i)
            if m and m.group(1) in reg:
                extern = True
            # 被别的值覆盖的寄存器不再带 TP(存储、比较、跳转没有目的寄存器)。跳转按名字精确认，
            # 免得把 bic/bfi 这类带目的寄存器的也跳过；独占存储的第一个操作数是写回的状态寄存器，照常丢
            if op in ("b", "bl", "blr", "br", "ret") or op.startswith(("b.", "cmp", "cmn", "tst", "cb", "tb")):
                continue
            if op.startswith("st") and not op.startswith(("stxr", "stlxr", "stxp", "stlxp")):
                continue
            for r in re.findall(r"\b(x\d+|w\d+)\b", i)[:2 if op in ("ldp", "ldpsw") else 1]:
                drop("x" + r[1:])
        for r in list(pend):
            drop(r)
        # 落在线程控制块(偏移 < ELF_TCB)里的是 libc 自己的字段，不算本项目变量
        seen = {v for v in seen if not (v.startswith("?+") and int(v[2:], 16) < ELF_TCB)}
        unres = {v for v in seen if v.startswith("?+")}
        hit |= seen - unres
        if ALLOW_FUNCS.search(name):
            continue
        vars_bad = {v for v in seen - unres if v not in ALLOW_VARS}
        if vars_bad:
            bad[name] = sorted(vars_bad)
        elif extern:
            warn[name] = "external library TLS"
        elif unres or not seen:
            warn[name] = "thread pointer read, variable not resolved"
    return bad, warn, hit


def check(path):
    head = open(path, "rb").read(20)
    warn = {}
    if head[:8] == b"\xcf\xfa\xed\xfe\x0c\x00\x00\x01":# 64 位 Mach-O，cputype arm64
        bad, hit = _scan_macho(_functions(path), _macho_vars(path))
    elif head[:6] == b"\x7fELF\x02\x01" and head[18:20] == b"\xb7\x00":# ELF64 小端，e_machine AArch64
        bad, warn, hit = _scan_elf(_functions(path), _elf_vars(path))
    else:
        print("%s: unsupported format (only arm64 Mach-O / aarch64 ELF)" % path)
        return 2
    if not hit:
        # 本项目产物至少有 TLS_DEFINE 的访问器；一处都没认出来就是被 strip、反汇编失败或格式不对
        print("%s: no TLS access resolved, cannot check" % path)
        return 2
    for name in sorted(warn):
        print("%s: warning: %s in %s" % (path, warn[name], name))
    for name in sorted(bad):
        print("%s: raw TLS %s in %s" % (path, ",".join(bad[name]), name))
    print("%s: %d function(s) with raw TLS" % (path, len(bad)))
    return 1 if bad else 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    return max(check(p) for p in sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
