-- 函数级热更新模块:替换 package.loaded[module] 表上的 function,通过 upvaluejoin 嫁接同名 upvalue 状态。
-- 约束:
--   1. 仅替换 module 表上的 M.xxx = function() end 形式;不支持新增/删除函数、不支持 metatable / C function
--   2. patch 必须用 `function M.xxx() end` 写法,patch_env 中 M 是代理表(__index = mod)
--   3. patch 想读/写原 module 顶层 local,两种用法(反复热修时不等价,见末两行):
--      - 路径 A:patch 用 `local name = ...` 重新声明同名 local,debug.upvaluejoin 在替换时嫁接
--        patch fn 的 name upvalue 槽到原 fn 同名 upvalue UpVal,两个 closure 共享同一份内存
--      - 路径 B:patch 裸读 `name`(不声明 local),走 patch_env metatable 转发到原 mod closure 的 upvalue,
--        读取/写入都作用于原 UpVal,所有引用 name 的旧 closure 与新 closure 同步看到新值
--      路径 A 替换后 patch fn 仍具名持有该 UpVal,可对同一函数反复热修;路径 B 替换后只有 _ENV,
--      若该 local 仅被本函数持有,二次热修 _collect_upvalues 扫不到 cell(转发回退 _G → nil)→ 反复迭代用路径 A
--   4. patch 中的 local function _helper 是 patch chunk 独立 closure;仅被 patch 内同时重写的 M.xxx 使用,
--      需要单独热修的 helper 应业务侧提升为 module 表字段(M._helper 而不是 local _helper)
--   5. 已 yield 的协程持有旧 closure reference,继续跑旧逻辑;新调用从 mod[name] 取走新版
-- 用法:
--   local hotfix = require("lib.hotfix")
--   local ok, msg = hotfix.apply("mymod", patch_source)

local M = {}

-- 把 patch_fn 的同名 upvalue 槽嫁接到 mod 内任一持有该 UpVal 的 closure(upmap 提供索引,路径 A)
-- 同 chunk 内 chunk-local 是单一 UpVal 对象,任意持有它的 closure 都能定位,不必限制嫁接到当前 orig_fn —
-- 例如 patch_handle 引用 counter 但原 handle 不引用 counter,counter UpVal 仍存在于原 bump,通过 upmap 命中
local function _join_upvalues(patch_fn, upmap)
    local pi = 1
    while true do
        local pname, _ = debug.getupvalue(patch_fn, pi)
        if not pname then break end
        -- _ENV 不嫁接:patch 与原 module 的 _ENV 是不同沙箱,共享会让 patch 写到原 module 全局
        if "_ENV" ~= pname then
            local entry = upmap[pname]
            if entry then
                debug.upvaluejoin(patch_fn, pi, entry.fn, entry.idx)
            end
        end
        pi = pi + 1
    end
end

-- 扫 mod 表所有 function 的 upvalue,收集 name → {fn, idx, id}(_ENV 排除)
-- upvalueid 辨 cell 身份:同名不同 cell(chunk 内 local 遮蔽)按名无法判定嫁接目标,标记 ambiguous 供 apply 拒绝
local function _collect_upvalues(mod)
    local map = {}
    for _, fn in pairs(mod) do
        if "function" == type(fn) then
            local i = 1
            while true do
                local name, _ = debug.getupvalue(fn, i)
                if not name then break end
                if "_ENV" ~= name then
                    local id = debug.upvalueid(fn, i)
                    local entry = map[name]
                    if nil == entry then
                        map[name] = {fn = fn, idx = i, id = id}
                    elseif entry.id ~= id then
                        entry.ambiguous = true
                    end
                end
                i = i + 1
            end
        end
    end
    return map
end

---对 module 应用 hotfix patch;成功后下次调用走新版,upvalue 状态保留。
---失败时回滚 path-B 在执行期写入原 module 的 upvalue(函数替换本就只在成功后进行),原 module 不被半改污染
---@param module_name string 已加载的 module 名(package.loaded 中的 key)
---@param patch_source string patch Lua 源码;用 `function M.xxx() end` 声明替换函数
---@return boolean ok 成功 true / 失败 false
---@return string detail 成功时为替换函数数描述,失败时为错误信息
function M.apply(module_name, patch_source)
    local mod = package.loaded[module_name]
    if not mod or "table" ~= type(mod) then
        return false, "module not loaded as table: " .. tostring(module_name)
    end
    if "string" ~= type(patch_source) then
        return false, "patch source not a string"
    end
    -- patch_M 代理表:patch 写 `function M.xxx() end` 落到 patch_M(__newindex 走 rawset);
    -- patch 读 `M._helper` 透传 mod(__index = mod),让 patch 内部能引用原 module 未替换字段
    local patch_M = setmetatable({}, {__index = mod})
    -- 收集 mod 所有 closure 的 upvalue 索引,供 patch_env metatable 转发(路径 B)
    local upmap = _collect_upvalues(mod)
    -- 含同名遮蔽 upvalue(同名不同 cell)时按名嫁接无法判定目标,拒绝处理避免静默接错 cell
    for _, entry in pairs(upmap) do
        if entry.ambiguous then
            return false, "module has shadowed upvalues, hotfix unsupported"
        end
    end
    -- path-B 写入原 module UpVal 的撤销日志:k → {entry, orig};任一失败路径逐一回滚,避免半改污染
    local dirty = {}
    local function _rollback_dirty()
        for _, d in pairs(dirty) do
            debug.setupvalue(d.entry.fn, d.entry.idx, d.orig)
        end
    end
    -- patch_env:M 注入 patch_M;裸标识符读写经 metatable 转发到原 UpVal(命中 upmap)
    -- 或退化到 _G(未命中,如 print/pairs 等);patch_env 自身字段(rawset 写入)优先
    local env = {M = patch_M}
    setmetatable(env, {
        __index = function(_, k)
            local entry = upmap[k]
            if entry then
                local _, v = debug.getupvalue(entry.fn, entry.idx)
                return v
            end
            return _G[k]
        end,
        __newindex = function(t, k, v)
            local entry = upmap[k]
            if entry then
                if nil == dirty[k] then
                    -- 首次写入前记录原值供失败回滚(orig 可能为 nil,包一层 table 以区分"未记录")
                    local _, orig = debug.getupvalue(entry.fn, entry.idx)
                    dirty[k] = {entry = entry, orig = orig}
                end
                debug.setupvalue(entry.fn, entry.idx, v)
                return
            end
            rawset(t, k, v)
        end,
    })
    local chunk, err = load(patch_source, "=hotfix:" .. module_name, "t", env)
    if not chunk then
        return false, "load: " .. tostring(err)
    end
    local ok, exec_err = pcall(chunk)
    if not ok then
        _rollback_dirty()
        return false, "exec: " .. tostring(exec_err)
    end
    -- 遍历 patch_M:同名 function 嫁接 upvalue 后写回 mod;只替换已有函数,不新增不删除
    -- 嫁接走 upmap(覆盖整个 mod 的 UpVal 索引),不局限于当前 orig_fn 自身的 upvalue 列表
    local replaced = 0
    for name, patch_fn in pairs(patch_M) do
        if "function" == type(patch_fn) and "function" == type(mod[name]) then
            _join_upvalues(patch_fn, upmap)
            mod[name] = patch_fn
            replaced = replaced + 1
        end
    end
    if 0 == replaced then
        _rollback_dirty()
        return false, "no matching function replaced"
    end
    return true, string.format("%d function(s) replaced", replaced)
end

return M
