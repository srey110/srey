using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace command.DB
{
    public class SQLite_Cmds : SQLite
    {
        public string name = "";
        public string cmd = "";
        public SQLite_Cmds()
        {
            table = "cmds";
            conn = DBConn.m_SQLiteConn;
        }
        public List<SQLite_Cmds> All()
        {
            return Read<SQLite_Cmds>("SELECT * FROM " + table);
        }
    }
}
