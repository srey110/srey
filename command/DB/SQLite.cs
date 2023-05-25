using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Data.SQLite;
using System.Windows.Forms;
using System.Data.Common;
//https://system.data.sqlite.org/index.html/doc/trunk/www/downloads.wiki

namespace command.DB
{
    public class SQLite : DBBase
    {
        protected SQLiteConnection conn = null;
        protected DbTransaction m_Transaction = null;
        public override bool CheckHave(string strSql)
        {
            SQLiteDataReader objMySqlRdr = null;

            try
            {
                SQLiteCommand objSqlCmd = new SQLiteCommand(strSql, conn);
                objMySqlRdr = objSqlCmd.ExecuteReader();
                if (null == objMySqlRdr)
                {
                    return false;
                }
                if (!objMySqlRdr.HasRows)
                {
                    return false;
                }

                return objMySqlRdr.Read();
            }
            finally
            {
                if (null != objMySqlRdr)
                {
                    objMySqlRdr.Close();
                }
            }
        }
        public override List<T> Read<T>(string strSql)
        {
            SQLiteDataReader objMySqlRdr = null;
            List<T> lstTable = new List<T>();
            var lstFields = GetAllField(this);

            try
            {
                SQLiteCommand objSqlCmd = new SQLiteCommand(strSql, conn);
                objMySqlRdr = objSqlCmd.ExecuteReader();
                if (null == objMySqlRdr)
                {
                    return lstTable;
                }
                if (!objMySqlRdr.HasRows)
                {
                    return lstTable;
                }

                while (objMySqlRdr.Read())
                {
                    T objTable = System.Activator.CreateInstance<T>();
                    foreach (var field in lstFields)
                    {
                        if ("table" != field.name
                            && "conn" != field.name
                            && "m_Transaction" != field.name)
                        {
                            var val = objMySqlRdr[field.name].ToString();
                            var member = objTable.GetType().GetField(field.name);
                            if (member.FieldType == typeof(int))
                            {
                                member.SetValue(objTable, int.Parse(val));
                            }
                            else if (member.FieldType == typeof(long))
                            {
                                member.SetValue(objTable, long.Parse(val));
                            }
                            else if (member.FieldType == typeof(float))
                            {
                                member.SetValue(objTable, float.Parse(val));
                            }
                            else if (member.FieldType == typeof(double))
                            {
                                member.SetValue(objTable, double.Parse(val));
                            }
                            else
                            {
                                member.SetValue(objTable, val);
                            }
                        }
                    }
                    lstTable.Add(objTable);
                }

                return lstTable;
            }
            finally
            {
                if (null != objMySqlRdr)
                {
                    objMySqlRdr.Close();
                }
            }
        }
        public override void DeleteBySql(string strSql)
        {
            SQLiteCommand objSqlCmd = new SQLiteCommand(strSql, conn);
            objSqlCmd.ExecuteNonQuery();
        }
        public override void BeginTransaction()
        {
            m_Transaction = conn.BeginTransaction();
        }
        public override void Commit()
        {
            m_Transaction.Commit();
        }
        public override void Rollback()
        {
            m_Transaction.Rollback();
        }
        protected override void Insert(List<DB_Fields> lstFields)
        {
            var strSql = InsertSql(lstFields);
            SQLiteDataReader objMySqlRdr = null;

            try
            {
                SQLiteCommand objSqlCmd = new SQLiteCommand(strSql, conn);
                objSqlCmd.ExecuteNonQuery();
                if (id == -1)
                {
                    id = (int)conn.LastInsertRowId;
                }
            }
            finally
            {
                if (null != objMySqlRdr)
                {
                    objMySqlRdr.Close();
                }
            }
        }
        protected override void Update(List<DB_Fields> lstFields)
        {
            string strSql = UpdateSql(lstFields);
            SQLiteCommand objSqlCmd = new SQLiteCommand(strSql, conn);
            objSqlCmd.ExecuteNonQuery();
        }
        protected override bool CheckId()
        {
            SQLiteDataReader objMySqlRdr = null;

            try
            {
                SQLiteCommand objSqlCmd = new SQLiteCommand("SELECT id FROM " + table + " WHERE id=" + id.ToString(), conn);
                objMySqlRdr = objSqlCmd.ExecuteReader();
                if (null == objMySqlRdr)
                {
                    return false;
                }
                if (!objMySqlRdr.HasRows)
                {
                    return false;
                }

                return objMySqlRdr.Read();
            }
            finally
            {
                if (null != objMySqlRdr)
                {
                    objMySqlRdr.Close();
                }
            }
        }
    }
}
