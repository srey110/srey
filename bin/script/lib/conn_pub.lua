-- mysql / pgsql / mongo / smtp 四个长连接客户端共用的连接生命周期：
-- 建链、ping 保活兼重连、主动断开。这三段的排队与代次维护四家逐字相同，
-- 差异只在各自的协议动作上，故收在这里一份。
--
-- 子类须做两件事：
--   1) ctor 里建好 C 层句柄之后调 conn_pub.init(self, handle)；
--   2) 实现三个钩子（都在锁内被调用，不要自己再进 self.serial，虽然可重入但没必要）：
--      _connect()  建链 + 协议握手，成功返 true；代次与 established 由本模块统一维护，钩子内不要动
--      _ping()     探活，服务端有正常响应返 true；不要在里面重连
--      _doquit()   发协议的断开命令并关 socket；连接已关（fd 为 INVALID_SOCK）时自行早退
--
-- 为什么句柄要在 init 里另存一份 self.conn：基类只在重连前取一次 sock_id，
-- 而四家各自的字段名不同（self.mysql / self.pg / self.smtp / self.mongo），
-- 统一改名要动上百处调用点，存个别名便宜得多。

local srey = require("lib.srey")
local pub = class("conn_pub")

---按名取 SSL 上下文；四家 ctor 开头逐字相同的一步，取不到即 ctor 失败直接抛。
---level 3 而不是 2：本函数被子类 ctor 调，多垫了一层，3 才能落回原先写在 ctor 里的
---error(msg, 2) 落到的同一帧。注意那一帧是 utils.lua 的 cls.new（class() 生成的 new 会
---调 instance:ctor(...)，自成一帧），不是业务调用点——要指到业务得用 4，四家 ctor 里其余的
---error(..., 2) 也都落在 cls.new 上，改就得一起改
---@param sslname string SSL 名；SSL_NAME.NONE 表示明文
---@return any ssl SSL 上下文，明文时为 nil
function pub.ssl(sslname)
    local ok, ssl = srey.ssl_qury(sslname)
    if not ok then
        error(string.format("ssl_qury not find ssl name %s", sslname), 3)
    end
    return ssl
end

---初始化连接生命周期所需的共用字段；须在 C 层句柄建好之后调用
---@param self any 子类实例
---@param handle any C 层连接句柄，本模块只用它取 sock_id
function pub.init(self, handle)
    self.conn = handle
    -- 连接代次：握手成功与 quit 各 +1。prepare 出来的 stmt / 事务 session 持有创建时的代次，
    -- 用前比对，连接换过之后服务端已清掉的旧句柄就能明确报失效而不是拿旧 id 去打新连接
    self.generation = 0
    -- 此刻是否连着。代次只表示"身份换过一次"，quit 也让它前进，
    -- 故排队醒来的 connect 不能只看代次，详见 _doconnect
    self.established = false
    -- 命令串行化执行器：多协程共用一条连接时按 FIFO 排队。建在 ctor 而非 connect——
    -- connect 会被 ping / selectdb 的重连路径重入，建在那儿会在重连时换掉执行器，
    -- 把排队者连同锁一起丢掉
    self.serial = srey.serial()
end

---建立连接并完成协议握手
---@return boolean ok 握手成功 true，失败 false。多协程并发调用时按 FIFO 串行，
---排在后面那个若发现连接已被前一个重建好（代次已变且仍连着）直接返 true，不再白拆一次
function pub:connect()
    -- 排队前记下代次：等锁期间别人可能已经把连接重建好了
    local gen = self.generation
    return srey.serial_ret(false, self.serial(self._doconnect, self, gen))
end
-- 整段握手在锁内：try_connect 会无条件覆写 sk.fd/skid，别人正在这条连接上发命令的话
-- 那个 socket 就被孤立了；握手期间连接也还不能收普通命令。
-- 代次统一在这里递增，子类的 _connect 只管返回成败
function pub:_doconnect(gen)
    -- 代次变了只说明别人动过这条连接，不等于它连着——quit 同样让代次前进，
    -- 故还得认 established，否则排在 quit 后面的这一个会对着已关的连接报成功
    if gen ~= self.generation
        and self.established then
        return true
    end
    local ok = self:_connect()
    if ok then
        self.generation = self.generation + 1
    end
    -- 失败也要落：_connect 里的 try_connect 已经无条件覆写过 sk.fd，原来那条连接不在了
    self.established = ok
    return ok
end

---连接保活：探活失败时自动重连，建议在执行命令前调用
---@return boolean ok 连接可用 true；探活失败时在锁内重连，重连也失败返 false
function pub:ping()
    return srey.serial_ret(false, self.serial(self._pingreconn, self))
end
-- 重连整段也在锁内：连接正在重建时别人不该往上发命令，而 fd/skid 换掉之后
-- 排队者醒来拿到的自然是新连接
function pub:_pingreconn()
    if not self:_ping() then
        local fd, skid = self.conn:sock_id()
        srey.sync_close(fd, skid)
        return self:connect()
    end
    return true
end

---主动断开连接；此后再调 ping() 会把连接重新建起来
-- 走锁：断开命令插进别人正在进行的交换会串包，随后的 sync_close 更会把对方半途的等待
-- 直接打断——一次响应已完整到达的命令会因此报失败
function pub:quit()
    self.serial(self._quitreset, self)
end
-- 代次照 connect 那样前进：quit 一样让服务端清掉本连接的 stmt / session，旧句柄必须一并失效。
-- 子类的 _doquit 只管发断开命令，代次与 established 统一在这里落
function pub:_quitreset()
    self:_doquit()
    self.generation = self.generation + 1
    self.established = false
end
-- 命令失败后就地拆连接的收尾：只落状态不发断开命令（连接已不干净，再发也是白发）。
-- 子类要在 _doconnect / _doquit 之外关连接时必须走这里，否则 established 会留下
-- "还连着"的假值，把 _doconnect 的短路变成对着已关的连接报成功
function pub:_closereset()
    local fd, skid = self.conn:sock_id()
    if INVALID_SOCK ~= fd then
        srey.sync_close(fd, skid)
    end
    self.generation = self.generation + 1
    self.established = false
end

return pub
