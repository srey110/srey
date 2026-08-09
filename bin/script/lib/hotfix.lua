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
--      判据是"嫁接时 patch 侧该槽是否已持有函数",而 Lua 把 `local function f` 与 `local f = <函数>`
--      编译成同一形态、运行期无从区分,故 patch 里**不要**给状态型 local 赋函数初值
--      (写 `local logger = print` 会被当成 helper 跳过嫁接,patch 从此绑到自己那份上,
--      原 module 的 M.set_logger 再怎么改都看不到);要默认值就留空由 path-B 读原值
--   5. 已 yield 的协程持有旧 closure reference,继续跑旧逻辑;新调用从 mod[name] 取走新版
-- 用法:
--   local hotfix = require("lib.hotfix")
--   local ok, msg = hotfix.apply("mymod", patch_source)

local M = {}

-- 同名遮蔽(同名不同 cell)时按名嫁接找不准目标,只在 patch 真的碰到那个名字时才拒绝。
-- 曾是"整个模块有一处遮蔽就整体拒收",而模块级工厂被调两次(local a, b = mk(), mk())
-- 就足以造出两个同名 cell,补丁明明只改别的名字也被挡在 load 之前
local _AMBIGUOUS = "patch touches shadowed upvalue: "
-- 补丁 chunk 的 chunkname 前缀，load 时给它；_is_self_chunk 也认这个形态——
-- 上一轮 apply 换上去的函数正是从补丁 chunk 编译出来的，反复热修同一模块时它就是"自家的"
local _PATCH_CHUNK = "=hotfix:"

-- 找 patch 里会走嫁接、而原模块侧同名 cell 有歧义的 upvalue;有则返回名字。
-- 判定与 _join_upvalues 的嫁接条件保持一致:只有非函数值才嫁接,函数值是 patch 自己的 helper
local function _find_ambiguous(patch_fn, upmap)
    local pi = 1
    while true do
        local pname, pval = debug.getupvalue(patch_fn, pi)
        if not pname then
            break
        end
        if "_ENV" ~= pname and "function" ~= type(pval) then
            local entry = upmap[pname]
            if entry and entry.ambiguous then
                return pname
            end
        end
        pi = pi + 1
    end
    return nil
end

-- 把 patch_fn 的同名 upvalue 槽嫁接到 mod 内任一持有该 UpVal 的 closure(upmap 提供索引,路径 A)
-- 同 chunk 内 chunk-local 是单一 UpVal 对象,任意持有它的 closure 都能定位,不必限制嫁接到当前 orig_fn —
-- 例如 patch_handle 引用 counter 但原 handle 不引用 counter,counter UpVal 仍存在于原 bump,通过 upmap 命中
local function _join_upvalues(patch_fn, upmap)
    local pi = 1
    while true do
        local pname, pval = debug.getupvalue(patch_fn, pi)
        if not pname then break end
        -- _ENV 不嫁接:patch 与原 module 的 _ENV 是不同沙箱,共享会让 patch 写到原 module 全局
        if "_ENV" ~= pname then
            local entry = upmap[pname]
            if entry then
                -- 嫁接只对"状态型"local 有意义(counter 之类,共享同一份内存跨热修保留)。
                -- 只看 patch 这一侧的当前值:patch 的 `local function _helper` 槽此刻必然持有
                -- 它自己新建的闭包,嫁接回原 cell 等于把新实现整个丢弃、悄悄换回旧的(约束 4);
                -- 而 patch 的状态型 `local x` 在 chunk 刚跑完时是 nil,照常嫁接。
                -- 不能连原模块侧一起判:原模块的 local 完全可能是"值恰好为函数"的回调槽
                -- (local cb; function M.set(f) cb = f end),那是状态不是代码,漏掉嫁接会让
                -- patch 绑到自己那份 nil 上,且是否漏掉还取决于热修时 cb 有没有被赋过值
                if "function" ~= type(pval) then
                    debug.upvaluejoin(patch_fn, pi, entry.fn, entry.idx)
                end
            end
        end
        pi = pi + 1
    end
end

-- 扫一个 closure 的 upvalue 收进 map,再顺着同 chunk 的函数型 upvalue 往下扫。
-- 必须往下扫:模块里 `local function _helper` 不是 mod 表的值,pairs(mod) 看不见它,
-- 而只被它捕获的模块级 local(local cache 之类)就进不了 map——patch 里读那个名字会
-- 一路回退到 _G 拿到 nil,写则落到 _G,而 apply 照样报"替换成功"。
-- 只跟同 chunk 的:`local cb; function M.set(f) cb = f end` 这种业务回调槽也是函数型 upvalue,
-- 顺着它扫进去就是把别人 closure 的 local 名字混进 map(Lua 把 `local function f` 与
-- `local f = <函数>` 编译成同一形态,运行期只能靠 source 分辨)。
-- src 由调用方一次算好往下传,不能每层拿当前 fn 重算:重算等于每跟一步就把基准挪到刚踩进去的
-- 那个 chunk 上,A 模块 re-export 了 B 的函数就顺势把 B 整棵闭包树扫进 A 的 map,
-- 补丁写同名变量会经 __newindex 落到 B 的 cell 上。
-- seen 防互相递归的 helper 打转
local function _scan_upvalues(fn, map, seen, src)
    if seen[fn] then
        return
    end
    seen[fn] = true
    local i = 1
    while true do
        local name, val = debug.getupvalue(fn, i)
        if not name then break end
        if "_ENV" ~= name then
            local id = debug.upvalueid(fn, i)
            local entry = map[name]
            if nil == entry then
                map[name] = {fn = fn, idx = i, id = id}
            elseif entry.id ~= id then
                entry.ambiguous = true
            end
            if "function" == type(val)
                and src == debug.getinfo(val, "S").source then
                _scan_upvalues(val, map, seen, src)
            end
        end
        i = i + 1
    end
end
-- 判定 src 是不是 module_name 自己那个 chunk。
-- 之所以要认准:mod 表里混得进别人编译的函数(A.helper = require"b".helper 这种 re-export,
-- 以及 utils 的 class() 塞进每个类表的 cls.new),拿它当出发点就会把那个模块整棵闭包树
-- 收进本模块的 map,补丁写同名变量就直接改到别人家的状态上去了。
-- 只比路径尾巴,不要求 "@" 前缀:同一个模块的 source 有两种形态——bytecache 未命中走
-- luaL_loadfilex 得到 "@路径",命中走 luaL_loadbufferx 而 chunkname 传的是裸路径(见
-- lbytecache.c);要求前缀会让所有走缓存的模块认不出自己。load(src,"=名字") 另走全等
local function _is_self_chunk(src, module_name)
    if "=" .. module_name == src
        or _PATCH_CHUNK .. module_name == src then
        return true
    end
    local tail = "/" .. module_name:gsub("%.", "/") .. ".lua"
    return tail == src:gsub("\\", "/"):sub(-#tail)
end
-- 收集 name → {fn, idx, id}(_ENV 排除)
-- upvalueid 辨 cell 身份:同名不同 cell(chunk 内 local 遮蔽)按名无法判定嫁接目标,标记 ambiguous;
-- 拒不拒推迟到 patch 真的碰那个名字时判,见 _AMBIGUOUS
-- 返回 map、mod 里的函数个数、其中出自本模块 chunk 的个数
local function _collect_upvalues(mod, module_name)
    local map = {}
    local seen = {}
    local nfn = 0
    local nself = 0
    for _, fn in pairs(mod) do
        if "function" == type(fn) then
            nfn = nfn + 1
            local src = debug.getinfo(fn, "S").source
            if _is_self_chunk(src, module_name) then
                nself = nself + 1
                _scan_upvalues(fn, map, seen, src)
            end
        end
    end
    return map, nfn, nself
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
    local upmap, nfn, nself = _collect_upvalues(mod, module_name)
    -- 有函数却一个都认不出是自家的:upmap 必然为空,补丁裸读写会全部悄悄落到 _G,
    -- 而 apply 照报"替换成功"。宁可在这里失败,也别让调用方以为状态改进去了
    if 0 < nfn and 0 == nself then
        return false, "cannot locate module chunk: " .. module_name
    end
    -- path-B 写入原 module UpVal 的撤销日志:k → {entry, orig};任一失败路径逐一回滚,避免半改污染
    local dirty = {}
    -- patch 写出的真全局同样要能撤销:只回滚 upvalue 的话,chunk 执行到一半抛错时调用方以为
    -- "原状态未被污染",而写出去的全局已经永久留在该 task 的 _G 里,后续任何裸标识符读都看得到
    local dirty_g = {}
    local function _rollback_dirty()
        for _, d in pairs(dirty) do
            debug.setupvalue(d.entry.fn, d.entry.idx, d.orig)
        end
        for k, d in pairs(dirty_g) do
            _G[k] = d.orig
        end
    end
    -- patch_env:M 注入 patch_M;裸标识符读写经 metatable 转发到原 UpVal(命中 upmap)
    -- 或退化到 _G(未命中,如 print/pairs 等);patch_env 自身字段(rawset 写入)优先
    local env = {M = patch_M}
    setmetatable(env, {
        __index = function(_, k)
            local entry = upmap[k]
            if entry then
                if entry.ambiguous then
                    error(_AMBIGUOUS .. k, 2)
                end
                local _, v = debug.getupvalue(entry.fn, entry.idx)
                return v
            end
            return _G[k]
        end,
        __newindex = function(t, k, v)
            local entry = upmap[k]
            if entry then
                if entry.ambiguous then
                    error(_AMBIGUOUS .. k, 2)
                end
                if nil == dirty[k] then
                    -- 首次写入前记录原值供失败回滚(orig 可能为 nil,包一层 table 以区分"未记录")
                    local _, orig = debug.getupvalue(entry.fn, entry.idx)
                    dirty[k] = {entry = entry, orig = orig}
                end
                debug.setupvalue(entry.fn, entry.idx, v)
                return
            end
            -- 未命中 upmap：与 __index 的 _G[k] 回退对称，写真正全局，而非困在一次性 env 沙箱里出不来
            if nil == dirty_g[k] then
                -- 同 dirty:orig 可能为 nil(该全局原本不存在),包一层 table 以区分"未记录"
                dirty_g[k] = {orig = _G[k]}
            end
            _G[k] = v
        end,
    })
    local chunk, err = load(patch_source, _PATCH_CHUNK .. module_name, "t", env)
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
    -- 先整体验一遍再动手:嫁接与写回是逐个进行的,做到一半才发现遮蔽就得回滚已经换上去的函数
    for name, patch_fn in pairs(patch_M) do
        if "function" == type(patch_fn) and "function" == type(mod[name]) then
            local bad = _find_ambiguous(patch_fn, upmap)
            if bad then
                _rollback_dirty()
                return false, _AMBIGUOUS .. bad
            end
        end
    end
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
