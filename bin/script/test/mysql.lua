local srey = require("lib.srey")
local mysql = require("lib.mysql")
local mbind = require("mysql.bind")

local function _onconnect(pktype, fd, skid, err)
    printd("connected mysql " .. pktype .. " " .. err)
end
local function print_reader(reader)
    printd("-----------------------------------------")
    local prt, ok, val, size, isneg, day, h, m, s
    while not reader:eof() do
        ok, val = reader:integer("t_int32")
        prt = val .. "   "
        ok, val = reader:double("t_double")
        prt = prt .. val .. "   "
        ok, val = reader:datetime("t_datetime")
        prt = prt .. val .. "   "
        ok, val, size = reader:string("t_string")
        prt = prt .. srey.ud_str(val, size) .. "   "
        ok, isneg, day, h, m, s = reader:time("t_time")
        prt = prt .. tostring(isneg) .." " .. day .. " " ..h .. " " .. m.." " ..s.. "   "
        printd(prt)
        reader:next()
    end
end
local function _timeout()
    local mctx = mysql.new("127.0.0.1", 3306, SSL_NAME.NONE, "admin", "12345678", "test", "utf8", 0)
    if not mctx:connect() then
        printd("mysql connect error")
    end
    printd("mysql version: " .. mctx:version())
    local rtn = mctx:selectdb("test1")
    if not rtn then
        printd(mctx:erro())
    end
    rtn = mctx:selectdb("test")
    if not rtn then
        printd(mctx:erro())
    end
    rtn = mctx:ping()
    if not rtn then
        printd("ping error")
    end
    mctx:quit()
    rtn = mctx:ping()
    if not rtn then
        printd("ping error")
    end
    local bind = mbind.new()
    for i = 1, 5, 1 do
        bind:clear()
        bind:integer("t_int8", 1 + i);
        bind:integer("t_int16", 2 + i);
        bind:integer("t_int32", 3 + i);
        bind:integer("t_int64", 4 + i);
        bind:double("t_float", 5.123456 + i);
        bind:double("t_double", 6.789 + i);
        bind:string("t_string", "this is test.");
        bind:datetime("t_datetime", os.time());
        bind:time("t_time", 0, 2, 15, 30, 29);
        bind:null("t_nil");
        local sql = "insert into test_bind (t_int8,t_int16,t_int32,t_int64,t_float,t_double, t_string,t_datetime,t_time,t_nil) values(mysql_query_attribute_string('t_int8'),mysql_query_attribute_string('t_int16'), mysql_query_attribute_string('t_int32'),mysql_query_attribute_string('t_int64'),mysql_query_attribute_string('t_float'),mysql_query_attribute_string('t_double'),mysql_query_attribute_string('t_string'),mysql_query_attribute_string('t_datetime'),mysql_query_attribute_string('t_time'),mysql_query_attribute_string('t_nil'))"
        rtn = mctx:query(sql, bind)
        if not rtn then
            printd(mctx:erro())
            break
        end
        print("last id:"..mctx:last_id())
    end

    local sql = "select * from test_bind"
    local reader = mctx:query(sql)
    if not reader then
        printd(mctx:erro())
    end
    print_reader(reader)

    local stmt = mctx:prepare("select * from test_bind")
    if not stmt then
        printd(mctx:erro())
    end
    reader = stmt:execute()
    print_reader(reader)

    stmt = mctx:prepare("select * from test_bind where t_int8=?")
    if not stmt then
        printd(mctx:erro())
    end
    bind:clear()
    bind:integer("t_int8", 2);
    reader = stmt:execute(bind)
    print_reader(reader)

    sql = "delete from test_bind"
    rtn = mctx:query(sql)
    if not reader then
        printd(mctx:erro())
    end
    print("affectd rows:"..mctx:affectd_rows())
end
srey.startup(
    function ()
        srey.on_connected(_onconnect)
        srey.timeout(1000, _timeout)
    end
)
