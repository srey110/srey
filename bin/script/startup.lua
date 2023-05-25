local srey = require("lib.srey")
require("test.test")

srey.sslevnew("server", "ca.crt", "sever.crt", "sever.key", SSL_FILETYPE.PEM)
srey.sslevp12new("client", "client.p12", "srey")

srey.register("lib/harbor.lua", TASK_NAME.HARBOR, 3)
srey.register("test/task1.lua", TASK_NAME.TAKS1, 3)
srey.register("test/task2.lua", TASK_NAME.TAKS2, 3)
srey.register("test/task3.lua", TASK_NAME.TAKS3, 3)

local task = srey.qury(TASK_NAME.TAKS1)
print("task name:" .. srey.name(task))
print("session:" .. srey.session(task))

