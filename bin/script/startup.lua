local srey = require("lib.srey")
require("test.test")

srey.sslevnew("server", "ca.crt", "sever.crt", "sever.key", SSLFILE_TYPE.PEM)
srey.sslevp12new("client", "client.p12", "srey")

srey.register("service.harbor", TASK_NAME.HARBOR, 3)
srey.register("test.task1", TASK_NAME.TAKS1, 3)
srey.register("test.task2", TASK_NAME.TAKS2, 3)
srey.register("test.task3", TASK_NAME.TAKS3, 3)

local task = srey.qury(TASK_NAME.TAKS1)
prind("task name:" .. srey.name(task))
prind("session:" .. srey.session(task))

