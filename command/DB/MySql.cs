using MySql.Data.MySqlClient;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace command.DB
{
    public class MySqlDB : DBBase
    {
        protected MySqlConnection conn = null;//连接
        protected MySqlTransaction m_Transaction = null;
        public override bool Ping()
        {
            return conn.Ping();
        }
        public override bool CheckHave(string strSql)
        {
            MySqlDataReader objMySqlRdr = null;

            try
            {
                MySqlCommand objSqlCmd = new MySqlCommand(strSql, conn);
                objMySqlRdr = objSqlCmd.ExecuteReader();
                if (null == objMySqlRdr)
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
            MySqlDataReader objMySqlRdr = null;
            List<T> lstTable = new List<T>();
            var lstFields = GetAllField(this);

            try
            {
                MySqlCommand objSqlCmd = new MySqlCommand(strSql, conn);
                objMySqlRdr = objSqlCmd.ExecuteReader();
                if (null == objMySqlRdr)
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
            MySqlCommand objSqlCmd = new MySqlCommand(strSql, conn);
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
            MySqlDataReader objMySqlRdr = null;

            try
            {
                MySqlCommand objSqlCmd = new MySqlCommand(strSql, conn);
                objSqlCmd.ExecuteNonQuery();
                if (id == -1)
                {
                    objSqlCmd = new MySqlCommand("SELECT LAST_INSERT_ID() as id", conn);
                    objMySqlRdr = objSqlCmd.ExecuteReader();
                    objMySqlRdr.Read();
                    id = int.Parse(objMySqlRdr["id"].ToString());
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
            MySqlCommand objSqlCmd = new MySqlCommand(strSql, conn);
            objSqlCmd.ExecuteNonQuery();
        }
        protected override bool CheckId()
        {
            MySqlDataReader objMySqlRdr = null;

            try
            {
                MySqlCommand objSqlCmd = new MySqlCommand("SELECT id FROM " + table + " WHERE id=" + id.ToString(), conn);
                objMySqlRdr = objSqlCmd.ExecuteReader();
                if (null == objMySqlRdr)
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
