using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace command.DB
{
    public class DBBase
    {
        protected class DB_Fields
        {
            public string name;
            public string value;
        }
        public int id = -1;//id每张表都要有
        protected string table = "";//表名
        protected List<DB_Fields> GetAllField(Object obj)
        {
            List<DB_Fields> lstName = new List<DB_Fields>();
            var members = obj.GetType().GetFields();
            foreach (var mem in members)
            {
                DB_Fields objInfo = new DB_Fields();
                objInfo.name = mem.Name;
                var val = mem.GetValue(obj);
                if (null != val)
                {
                    objInfo.value = val.ToString();
                }
                lstName.Add(objInfo);
            }

            return lstName;
        }
        public virtual bool Ping()
        {
            return true;
        }
        public virtual bool CheckHave(string strSql)
        {
            return true;
        }
        public virtual List<T> Read<T>(string strSql)
        {
            return null;
        }
        public virtual void DeleteBySql(string strSql)
        { }
        public virtual void BeginTransaction()
        { }
        public virtual void Commit()
        { }
        public virtual void Rollback()
        { }
        public void DeleteAll()
        {
            DeleteBySql("DELETE FROM " + table);
        }
        public void Delete()
        {
            if (-1 == id)
            {
                return;
            }

            string strSql = "DELETE FROM " + table + " WHERE id=" + id.ToString();
            DeleteBySql(strSql);
        }
        public string GetInsertSql()
        {
            var lstFields = GetAllField(this);
            return InsertSql(lstFields);
        }
        protected string InsertSql(List<DB_Fields> lstFields)
        {
            string strSql1 = "INSERT INTO " + table + " (";
            string strSql2 = "VALUES(";
            foreach (var filed in lstFields)
            {
                if ("id" != filed.name
                    && "table" != filed.name
                    && "conn" != filed.name
                    && "m_Transaction" != filed.name)
                {
                    strSql1 += filed.name + ",";
                    strSql2 += "'" + filed.value + "',";
                }
            }
            strSql1 = strSql1.Remove(strSql1.LastIndexOf(","), 1);
            strSql2 = strSql2.Remove(strSql2.LastIndexOf(","), 1);
            if (id != -1)
            {
                strSql1 += ", id";
                strSql2 += ", " + id.ToString();
            }
            strSql1 += ")";
            strSql2 += ")";

            return strSql1 + strSql2;
        }
        protected virtual void Insert(List<DB_Fields> lstFields)
        { }
        protected string UpdateSql(List<DB_Fields> lstFields)
        {
            string strSql = "UPDATE " + table + " SET ";
            foreach (var filed in lstFields)
            {
                if ("id" != filed.name
                    && "table" != filed.name
                    && "conn" != filed.name
                    && "m_Transaction" != filed.name)
                {
                    strSql += filed.name + "='" + filed.value + "',";
                }
            }
            strSql = strSql.Remove(strSql.LastIndexOf(","), 1);
            strSql += " WHERE id=" + id.ToString();

            return strSql;
        }
        protected virtual void Update(List<DB_Fields> lstFields)
        { }
        protected virtual bool CheckId()
        {
            return true;
        }
        public void Save()
        {
            var lstFields = GetAllField(this);
            if (-1 == id)
            {
                Insert(lstFields);
            }
            else
            {
                if (CheckId())
                {
                    Update(lstFields);
                }
                else
                {
                    Insert(lstFields);
                }
            }
        }
    }
}
