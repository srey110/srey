local srey = require("lib.srey")
srey.startup(
    function ()
        printd("%s", "task startup_closing run startup.")
    end
)
srey.closing(
    function ()
        printd("%s", "task startup_closing run closing.")
    end
)
