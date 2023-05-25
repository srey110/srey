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
        private bool m_bLinked = false;
        private Config m_Config = new Config();
        public FmMain()
        {
            InitializeComponent();
            txt_HarborIp.Text = m_Config.Configs.Harbor.Ip;
            txt_HarborPort.Text = m_Config.Configs.Harbor.Port.ToString();
            try
            {
                DBConn.m_SQLiteConn = new SQLiteConnection("Data Source=" + Application.StartupPath + "\\command\\command.s3db;");
                DBConn.m_SQLiteConn.Open();
            }
            catch (Exception e)
            {
                MessageBox.Show(e.Message);
            }
        }
        private void FmMain_FormClosed(object sender, FormClosedEventArgs e)
        {
            m_Config.Save();
        }
        private void bt_HarborLink_Click(object sender, EventArgs e)
        {
            if (!m_bLinked)
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
            }
            else
            {

            }
        }
    }
}
