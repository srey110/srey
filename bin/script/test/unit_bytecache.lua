-- 字节码缓存(lualib/lbind/lbytecache.c)的 require searcher 用例：
-- 改过的模块文件再 require 拿到新内容(版本戳拼了文件长度，同一秒内改也能失效)，没改的照常命中；
-- 找不到的模块，报错串与标准 searcher 一致("no file '...'"，模块名里的 '.' 换成目录分隔符)。
-- lbc_clear 没有 Lua 绑定，这里测不到。没开 ENABLE_LUA_BYTECACHE 时走标准 searcher，断言照样成立

local srey   = require("lib.srey")
local runner = require("test.runner")

local MOD = "unit_bytecache_tmp"-- 运行时写进脚本目录的临时模块，测完删掉
local NOMOD = "unit_bytecache_none.sub"-- 不存在的模块

---整份写入 path；打不开或写失败返回 false
---@param path string
---@param src string
---@return boolean ok
local function _write(path, src)
    local f = io.open(path, "wb")
    if nil == f then
        return false
    end
    local ok = f:write(src)
    f:close()
    return nil ~= ok
end

---清掉 package.loaded 里的 name 再 require 一次
---@param name string
---@return boolean ok
---@return any ret 模块返回值；失败时为错误串
local function _reload(name)
    package.loaded[name] = nil
    return pcall(require, name)
end

srey.startup(function()
runner.run(function(t)
    -- 第一个模板就是脚本目录(ltask.c 的 _ltask_setpath 把它加在最前)
    local tmpl = package.path:match("^[^;]+")
    if not t:check(nil ~= tmpl, "package.path 有模板") then
        return
    end

    -- ── 未命中：报错串列出脚本目录下试过的文件 ─────────────────────────
    do
        local ok, err = _reload(NOMOD)
        local tried = "no file '" .. tmpl:gsub("%?", (NOMOD:gsub("%.", _pathsep))) .. "'"
        t:check(false == ok and nil ~= tostring(err):find(tried, 1, true),
                "require 不存在的模块报错，报错串含 " .. tried)
    end

    -- ── 改过的文件重新 require 拿到新内容，没改的照常命中 ─────────────
    -- 收尾排在断言之前：断言失败会中断本块，临时文件就留在脚本目录里了
    local path = tmpl:gsub("%?", MOD)
    if _write(path, "return 'a'") then
        local _, v1 = _reload(MOD)
        local v2 = "write failed"
        if _write(path, "return 'bb'") then
            _, v2 = _reload(MOD)
        end
        local _, v3 = _reload(MOD)
        os.remove(path)
        package.loaded[MOD] = nil
        t:eq("a", v1, "首次 require 读到文件内容")
        t:eq("bb", v2, "改成不同长度后再 require 读到新内容(同一秒内改也失效)")
        t:eq("bb", v3, "文件没变时再 require 仍是新内容")
    else
        printd("[%s] script dir not writable, cache case skipped", t.name)
    end
end)
end)
