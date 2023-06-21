local srey = require("lib.srey")
require("test.test")

srey.evssl_new("server", "ca.crt", "sever.crt", "sever.key", SSLFILE_TYPE.PEM)
srey.evssl_p12new("client", "client.p12", "srey")

srey.task_register("service.harbor", TASK_NAME.HARBOR)
srey.task_register("test.task1", TASK_NAME.TASK1)
srey.task_register("test.task2", TASK_NAME.TASK2)
srey.task_register("test.task3", TASK_NAME.TASK3)
srey.task_register("test.task4", TASK_NAME.TASK4)
