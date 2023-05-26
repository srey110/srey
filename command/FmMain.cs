using command.DB;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Data.SQLite;
using System.Drawing;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace command
{
    public partial class FmMain : Form
    {
        private Config m_Config = new Config();
        private List<SQLite_Tasks> m_lstTasks = new List<SQLite_Tasks>();
        public FmMain()
        {
            InitializeComponent();
            txt_HarborIp.Text = m_Config.Configs.Harbor.Ip;
            txt_HarborPort.Text = m_Config.Configs.Harbor.Port.ToString();
            try
            {
                DBConn.m_SQLiteConn = new SQLiteConnection("Data Source=" + Application.StartupPath + "\\command\\command.s3db;");
                DBConn.m_SQLiteConn.Open();
                m_lstTasks = new DB.SQLite_Tasks().All();
                InitTaskName();
            }
            catch (Exception e)
            {
                MessageBox.Show(e.Message);
            }
        }
        private int GetTaskValue(string strTask)
        {
            foreach (var task in m_lstTasks)
            {
                if (task.name == strTask)
                {
                    return task.value;
                }
            }
            return -1;
        }
        private void InitTaskName()
        {
            string strSelect = cb_HarborTask.Text;
            cb_HarborTask.Items.Clear();
            foreach (var task in m_lstTasks)
            {
                cb_HarborTask.Items.Add(task.name);
            }
            if ("" != strSelect)
            {
                cb_HarborTask.SelectedIndex = cb_HarborTask.FindString(strSelect);
            }
            else
            {
                if (cb_HarborTask.Items.Count > 0)
                {
                    cb_HarborTask.SelectedIndex = 0;
                }
            }
        } 
        private void 任务ToolStripMenuItem_Click(object sender, EventArgs e)
        {
            FmTasks objTask = new FmTasks(m_lstTasks);
            objTask.ShowDialog();
            InitTaskName();
        }
        private void FmMain_FormClosed(object sender, FormClosedEventArgs e)
        {
            m_Config.Save();
            DBConn.m_SQLiteConn.Close();
        }
        private void bt_HarborLink_Click(object sender, EventArgs e)
        {
            string strIp;
            int iPort;
            try
            {
                strIp = txt_HarborIp.Text;
                iPort = int.Parse(txt_HarborPort.Text);
            }
            catch (Exception err)
            {
                MessageBox.Show(err.Message);
                return;
            }
            m_Config.Configs.Harbor.Ip = strIp;
            m_Config.Configs.Harbor.Port = iPort;

            bt_HarborLink.Enabled = false;
            bt_HarborRun.Enabled = true;
        }
        private void bt_HarborSave_Click(object sender, EventArgs e)
        {

        }
        private void bt_HarborDel_Click(object sender, EventArgs e)
        {

        }
        private void bt_HarborNew_Click(object sender, EventArgs e)
        {

        }
        private void bt_HarborRun_Click(object sender, EventArgs e)
        {

        }
    }
}
