#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""扫描 lualib/lbind/*.c 中的 XML 注释，生成 LuaCATS 类型存根到 bin/script/meta/。

用法（项目根目录运行）：
    python3 tools/gen_meta.py
"""
from __future__ import annotations

import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LBIND_DIR = ROOT / "lualib" / "lbind"
OUT_DIR = ROOT / "bin" / "script" / "meta"
LUALIB_HEADER = ROOT / "lualib" / "lua" / "lualib.h"
LINIT_SRC = ROOT / "lualib" / "lua" / "linit.c"


def build_require_map():
    """解析 linit.c + lualib.h，构建 luaopen_X → "srey.task" 等真实 require 名映射。"""
    require_map = {}
    if not (LUALIB_HEADER.is_file() and LINIT_SRC.is_file()):
        return require_map
    header = read_text(LUALIB_HEADER)
    init = read_text(LINIT_SRC)
    # 从 lualib.h 提取 #define LUA_X "name"
    macros = {}
    for m in re.finditer(r'#define\s+(LUA_[A-Za-z0-9_]+)\s+"([^"]+)"', header):
        macros[m.group(1)] = m.group(2)
    # 从 linit.c 提取 { LUA_X, luaopen_y }
    for m in re.finditer(r"\{\s*(LUA_[A-Za-z0-9_]+)\s*,\s*luaopen_([A-Za-z0-9_]+)\s*\}", init):
        macro = m.group(1)
        fn = m.group(2)
        if macro in macros:
            require_map[fn] = macros[macro]
    return require_map


def read_text(path: Path) -> str:
    data = path.read_bytes()
    if data.startswith(b"\xef\xbb\xbf"):
        data = data[3:]
    return data.decode("utf-8")


# 匹配单个 XML 块中的所有 /// 行，剥离前缀
def strip_slashes(block: str) -> str:
    out = []
    for line in block.splitlines():
        m = re.match(r"\s*///\s?(.*)$", line)
        out.append(m.group(1) if m else "")
    return "\n".join(out)


def xml_unescape(s: str) -> str:
    """将 XML 属性值中的实体引用还原为普通字符。"""
    return (s.replace("&lt;", "<").replace("&gt;", ">")
             .replace("&amp;", "&").replace("&quot;", '"').replace("&apos;", "'"))


def parse_xml_block(block: str) -> dict:
    """解析 XML 注释块。返回 {summary, params:[{name,type,desc}], returns:[{type,desc}]}
    type 属性值中的 &lt;/&gt; 会还原为 </> 以支持泛型写法（如 table&lt;string,string&gt;）。
    """
    text = strip_slashes(block)
    doc = {"summary": "", "params": [], "returns": []}

    m = re.search(r"<summary>\s*(.*?)\s*</summary>", text, re.DOTALL)
    if m:
        doc["summary"] = re.sub(r"\s*\n\s*", " ", m.group(1)).strip()

    for tag in re.finditer(r"<param([^>]*)>(.*?)</param>", text, re.DOTALL):
        attrs = tag.group(1)
        inner = re.sub(r"\s*\n\s*", " ", tag.group(2)).strip()
        name_m = re.search(r'name\s*=\s*"([^"]*)"', attrs)
        type_m = re.search(r'type\s*=\s*"([^"]*)"', attrs)
        name = name_m.group(1) if name_m else None
        typ = xml_unescape(type_m.group(1)) if type_m else None
        # 空参标记 <param>无</param>
        if name is None and typ is None and inner in ("无", ""):
            continue
        doc["params"].append({"name": name, "type": typ, "desc": inner})

    for tag in re.finditer(r"<returns([^>]*)>(.*?)</returns>", text, re.DOTALL):
        attrs = tag.group(1)
        inner = re.sub(r"\s*\n\s*", " ", tag.group(2)).strip()
        type_m = re.search(r'type\s*=\s*"([^"]*)"', attrs)
        typ = xml_unescape(type_m.group(1)) if type_m else None
        if typ is None and inner in ("无", ""):
            continue
        doc["returns"].append({"type": typ, "desc": inner})

    return doc


# 抓取所有 static int(32_t)? _xxx(lua_State *...) { 位置 + 前置 XML 块
def extract_func_docs(text: str) -> dict:
    fns = {}
    # 找到所有定义起点
    pattern = re.compile(
        r"static\s+int(?:32_t)?\s+(_[A-Za-z_][A-Za-z0-9_]*)\s*\(\s*lua_State\s*\*"
    )
    for m in pattern.finditer(text):
        fname = m.group(1)
        # 向上回溯收集连续的 /// 行
        before = text[: m.start()].rstrip()
        # 按行从下往上收集
        lines = before.splitlines()
        block_lines = []
        for line in reversed(lines):
            if re.match(r"\s*///", line):
                block_lines.append(line)
            else:
                break
        if block_lines:
            block_lines.reverse()
            block = "\n".join(block_lines)
            fns[fname] = parse_xml_block(block)
    return fns


def extract_mt_defines(text: str) -> dict:
    out = {}
    for m in re.finditer(r'#define\s+(MT_[A-Za-z0-9_]+)\s+"([^"]+)"', text):
        out[m.group(1)] = m.group(2)
    return out


def extract_reg_tables(text: str) -> dict:
    """提取所有 luaL_Reg <id>[] = { {"name", _func}, ... };"""
    out = {}
    for m in re.finditer(
        r"luaL_Reg\s+([A-Za-z_][A-Za-z0-9_]*)\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;",
        text,
        re.DOTALL,
    ):
        rid = m.group(1)
        body = m.group(2)
        entries = []
        for em in re.finditer(
            r'\{\s*"([^"]+)"\s*,\s*(_[A-Za-z0-9_]+)\s*\}', body
        ):
            entries.append({"name": em.group(1), "func": em.group(2)})
        out[rid] = entries
    return out


def extract_luaopen_bodies(text: str) -> list:
    """切出每个 LUAMOD_API int luaopen_xxx(...) { ... } 的函数体。"""
    out = []
    header_re = re.compile(
        r"LUAMOD_API\s+int\s+luaopen_([A-Za-z0-9_]+)\s*\(\s*lua_State\s*\*[^)]*\)\s*\{"
    )
    pos = 0
    while True:
        m = header_re.search(text, pos)
        if not m:
            break
        depth = 1
        i = m.end()
        while i < len(text) and depth > 0:
            ch = text[i]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
            i += 1
        body = text[m.end(): i - 1]
        out.append({"name": m.group(1), "body": body})
        pos = i
    return out


def classify_module(body: str, _unused: dict, mt_map: dict) -> dict:
    """识别模块级函数和元表方法。
    reg 表从 body 内局部提取，避免同名 reg_new / reg_func 被后续 luaopen 覆盖。"""
    reg_tables = extract_reg_tables(body)
    result = {"module": [], "classes": {}}

    for m in re.finditer(
        r"luaL_newlib\s*\(\s*lua\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)", body
    ):
        rid = m.group(1)
        for ent in reg_tables.get(rid, []):
            result["module"].append(ent)

    for m in re.finditer(
        r"REG_MTABLE\s*\(\s*lua\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,"
        r"\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)",
        body,
    ):
        mt_tok, nid, fid = m.group(1), m.group(2), m.group(3)
        cls_name = mt_map.get(mt_tok, mt_tok)
        for ent in reg_tables.get(nid, []):
            result["module"].append(ent)
        result["classes"].setdefault(cls_name, []).extend(
            reg_tables.get(fid, [])
        )

    # luaL_newmetatable(lua, MT_X); ... luaL_setfuncs(lua, REG_ID, 0);
    # 简化处理：找 newmetatable 后紧跟的 setfuncs
    for m in re.finditer(
        r"luaL_newmetatable\s*\(\s*lua\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)",
        body,
    ):
        mt_tok = m.group(1)
        cls_name = mt_map.get(mt_tok, mt_tok)
        after = body[m.end():]
        sf = re.search(
            r"luaL_setfuncs\s*\(\s*lua\s*,\s*([A-Za-z_][A-Za-z0-9_]*)", after
        )
        if sf:
            rid = sf.group(1)
            result["classes"].setdefault(cls_name, []).extend(
                reg_tables.get(rid, [])
            )

    # fallback：三种标准模式(newlib/REG_MTABLE/newmetatable)均未命中时，
    # 解析 lua_createtable 模块表上的注册
    if not result["module"] and not result["classes"]:
        for sm in re.finditer(
            r'lua_pushcfunction\s*\(\s*lua\s*,\s*(_[A-Za-z0-9_]+)\s*\)\s*;\s*'
            r'lua_setfield\s*\(\s*lua\s*,\s*-2\s*,\s*"([^"]+)"\s*\)',
            body,
        ):
            cfunc, name = sm.group(1), sm.group(2)
            if not name.startswith("__"):
                result["module"].append({"name": name, "func": cfunc})
        for sm in re.finditer(
            r"luaL_setfuncs\s*\(\s*lua\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*,", body
        ):
            for ent in reg_tables.get(sm.group(1), []):
                result["module"].append(ent)

    return result


def render_func(name: str, target: str, doc: dict | None, is_method: bool) -> str:
    lines: list[str] = []
    if doc and doc.get("summary"):
        lines.append("---" + doc["summary"])
    arg_names: list[str] = []
    if doc:
        skipped_self = False
        for p in doc.get("params", []):
            pname = p.get("name") or "_"
            if is_method and not skipped_self and pname == "self":
                skipped_self = True
                continue
            ptype = p.get("type") or "any"
            pdesc = p.get("desc") or ""
            if pname == "...":
                lines.append(f"---@param ... {ptype} {pdesc}".rstrip())
                arg_names.append("...")
            else:
                lines.append(f"---@param {pname} {ptype} {pdesc}".rstrip())
                arg_names.append(pname)
        for r in doc.get("returns", []):
            rtype = r.get("type") or "any"
            rdesc = r.get("desc") or ""
            lines.append(f"---@return {rtype} {rdesc}".rstrip())
    arglist = ", ".join(arg_names)
    sep = ":" if is_method else "."
    lines.append(f"function {target}{sep}{name}({arglist}) end")
    return "\n".join(lines)


# Lua 标识符不允许的字符替换
def lua_ident(s: str) -> str:
    s2 = re.sub(r"[^A-Za-z0-9_]", "_", s)
    if not s2:
        s2 = "_"
    if re.match(r"^[0-9]", s2):
        s2 = "_" + s2
    return s2


# Lua 中是关键字，不能用作 method 名（需用 ["end"] = function() 等方式）
LUA_KEYWORDS = {
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function",
    "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return",
    "then", "true", "until", "while",
}


def render_method(name: str, target: str, doc: dict | None) -> str:
    """metatable 方法：若 name 是 Lua 关键字，用 [\"name\"] 形式。"""
    if name in LUA_KEYWORDS:
        # 用赋值语法：target["end"] = function(self, x) ... end
        lines: list[str] = []
        if doc and doc.get("summary"):
            lines.append("---" + doc["summary"])
        arg_names = ["self"]
        if doc:
            skipped_self = False
            for p in doc.get("params", []):
                pname = p.get("name") or "_"
                if not skipped_self and pname == "self":
                    skipped_self = True
                    continue
                ptype = p.get("type") or "any"
                pdesc = p.get("desc") or ""
                if pname == "...":
                    lines.append(f"---@param ... {ptype} {pdesc}".rstrip())
                    arg_names.append("...")
                else:
                    lines.append(f"---@param {pname} {ptype} {pdesc}".rstrip())
                    arg_names.append(pname)
            for r in doc.get("returns", []):
                rtype = r.get("type") or "any"
                rdesc = r.get("desc") or ""
                lines.append(f"---@return {rtype} {rdesc}".rstrip())
        arglist = ", ".join(arg_names)
        lines.append(f'{target}["{name}"] = function({arglist}) end')
        return "\n".join(lines)
    return render_func(name, target, doc, is_method=True)


def gen_module(modname: str, classified: dict, func_map: dict, src: Path,
               require_map: dict) -> str:
    # luaopen_X 的 X 可能与 require 名不同；优先用 require_map 里的真名
    require_name = require_map.get(modname, modname)
    out = [f"---@meta {require_name}",
           f"-- 由 tools/gen_meta.py 自动生成，请勿手工修改",
           f"-- 来源：{src.relative_to(ROOT)}",
           ""]

    class_names = sorted(classified["classes"].keys())
    for cn in class_names:
        var = lua_ident(cn)
        out.append(f"---@class {cn}")
        out.append(f"local {var} = {{}}")
        out.append("")
        for ent in sorted(classified["classes"][cn], key=lambda e: e["name"]):
            # 跳过元方法 __gc / __index / __newindex / __tostring 等
            if ent["name"].startswith("__"):
                continue
            doc = func_map.get(ent["func"])
            out.append(render_method(ent["name"], var, doc))
            out.append("")

    out.append("local M = {}")
    out.append("")
    for ent in sorted(classified["module"], key=lambda e: e["name"]):
        doc = func_map.get(ent["func"])
        out.append(render_func(ent["name"], "M", doc, is_method=False))
        out.append("")

    out.append("return M")
    out.append("")
    return "\n".join(out)


def main() -> int:
    if not LBIND_DIR.is_dir():
        sys.stderr.write(f"missing dir: {LBIND_DIR}\n")
        return 1
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    require_map = build_require_map()

    c_files = sorted(LBIND_DIR.glob("*.c"))
    if not c_files:
        sys.stderr.write(f"no .c file found under {LBIND_DIR}\n")
        return 1

    total = 0
    for path in c_files:
        text = read_text(path)
        mt_map = extract_mt_defines(text)
        func_map = extract_func_docs(text)
        reg_tables = extract_reg_tables(text)
        bodies = extract_luaopen_bodies(text)
        for mb in bodies:
            classified = classify_module(mb["body"], reg_tables, mt_map)
            content = gen_module(mb["name"], classified, func_map, path, require_map)
            # 文件名沿用 luaopen_X 的 X（与 require 名解耦，避免 . 出现在路径里）
            out_path = OUT_DIR / f"{mb['name']}.lua"
            out_path.write_text(content, encoding="utf-8")
            print(f"generated {out_path.relative_to(ROOT)}")
            total += 1
    print(f"done, {total} module(s) generated.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
