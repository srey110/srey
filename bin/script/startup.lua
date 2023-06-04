local srey = require("lib.srey")
require("test.test")

srey.sslevnew("server", "ca.crt", "sever.crt", "sever.key", SSLFILE_TYPE.PEM)
srey.sslevp12new("client", "client.p12", "srey")

srey.register("service.harbor", TASK_NAME.HARBOR)
srey.register("test.task1", TASK_NAME.TASK1)
srey.register("test.task2", TASK_NAME.TASK2)
srey.register("test.task3", TASK_NAME.TASK3)
srey.register("test.task4", TASK_NAME.TASK4)

local task = srey.qury(TASK_NAME.TASK1)
printd("task name:" .. srey.name(task))
printd("session:" .. srey.session(task))
