-- srey.closing(CLOSING 生命周期回调)测试用探针。
-- mode=0:closing 里 srey.call 回 parent(reqtype 200),证明回调真被调到。
-- mode=1:closing 里故意抛错,parent 据此断言 task 仍然消失 —— 回调抛错时 srey.lua 的
--        _closing 仍须走 task_ungrab,漏掉就是 ref 不归零、_loader_task_closing 死等、Ctrl+C 收不了进程。
-- mode=2:另挂两个 srey.watch_closing 观察者,closing 先 call 回 "closing" 再抛错,两个观察者依次 call 回
--        "w1" "w2"(reqtype 201),parent 据此断言先业务后观察者、按注册顺序、业务抛错不挡观察者。
-- 注册参数:task.register("test.closing_probe", NAME, 0, parent_name, mode)

local srey = require("lib.srey")

local _parent, _mode = ...

srey.startup(function()
    if 2 == _mode then
        srey.watch_closing(function() srey.call(_parent, 201, "w1") end)
        srey.watch_closing(function() srey.call(_parent, 201, "w2") end)
    end
    srey.closing(function()
        if 2 == _mode then
            srey.call(_parent, 201, "closing")
            error("closing raise on purpose")
        end
        if 1 == _mode then
            error("closing raise on purpose")
        end
        srey.call(_parent, 200, "closed")
    end)
end)
