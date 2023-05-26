using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace command.DB
{
    public class SQLite_Tasks : SQLite
    {
        public string name = "";
        public int value = 0;
        public SQLite_Tasks()
        {
            table = "tasks";
            conn = DBConn.m_SQLiteConn;
        }
        public List<SQLite_Tasks> All()
        {
            return Read<SQLite_Tasks>("SELECT * FROM " + table);
        }
    }
}
