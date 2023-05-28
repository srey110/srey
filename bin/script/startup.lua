local srey = require("lib.srey")
require("test.test")

srey.sslevnew("server", "ca.crt", "sever.crt", "sever.key", SSLFILE_TYPE.PEM)
srey.sslevp12new("client", "client.p12", "srey")

srey.register("service.harbor", TASK_NAME.HARBOR)
srey.register("test.task1", TASK_NAME.TAKS1)
srey.register("test.task2", TASK_NAME.TAKS2)
srey.register("test.task3", TASK_NAME.TAKS3)

local task = srey.qury(TASK_NAME.TAKS1)
printd("task name:" .. srey.name(task))
printd("session:" .. srey.session(task))
